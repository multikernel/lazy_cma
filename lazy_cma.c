// SPDX-License-Identifier: GPL-2.0
/*
 * lazy_cma - Runtime Contiguous Memory Allocator
 *
 * Like CMA, but without boot-time reservation. Allocates physically
 * contiguous memory on demand using alloc_contig_range() and exposes
 * it via /dev/lazy_cma ioctls.
 *
 * Use cases:
 *   - daxfs: physically contiguous backing store
 *   - multikernel: spawn kernel memory pool
 *   - kdump: crash kernel reservation without crashkernel= boot param
 *
 * Copyright (C) 2026 Multikernel Technologies, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "lazy_cma: " fmt

#include <linux/err.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/ioctl.h>

#define LAZY_CMA_IOC_MAGIC		'H'

#define LAZY_CMA_NAME_MAX	64

struct lazy_cma_allocation_data {
	__u64 len;
	__u64 phys_addr;	/* out: physical address of allocation */
	__s32 node;		/* NUMA node; -1 = any */
	__u32 pad;
	char name[LAZY_CMA_NAME_MAX];	/* iomem resource name; empty = "lazy_cma" */
};

#define LAZY_CMA_IOCTL_ALLOC	_IOWR(LAZY_CMA_IOC_MAGIC, 0x0, \
				      struct lazy_cma_allocation_data)

struct lazy_cma_resize_data {
	__u64 phys_addr;	/* identifies the buffer; updated on reallocation */
	__u64 len;		/* new size */
};

#define LAZY_CMA_IOCTL_RESIZE	_IOWR(LAZY_CMA_IOC_MAGIC, 0x1, \
				      struct lazy_cma_resize_data)
#define LAZY_CMA_IOCTL_FREE	_IOW(LAZY_CMA_IOC_MAGIC, 0x2, __u64)

struct lazy_cma_buffer {
	struct list_head list;		/* global buffer list */
	unsigned long len;
	struct page *pages;		/* first page of contiguous range */
	unsigned long nr_pages;
	int node;			/* NUMA node; -1 = any */
	const char *name;		/* iomem resource name */
	phys_addr_t phys_addr;
	struct resource iomem;
};

static LIST_HEAD(lazy_cma_buffers);
static DEFINE_MUTEX(lazy_cma_buffers_lock);

/*
 * Try to allocate contiguous pages by scanning zones for a range that
 * alloc_contig_range() can satisfy. We walk pageblock-aligned ranges
 * from the top of each zone downward (higher addresses are less likely
 * to contain pinned kernel allocations).
 *
 * Zone scan order: ZONE_MOVABLE first (guaranteed no pinned pages),
 * then ZONE_NORMAL as fallback.
 */
static struct page *lazy_cma_try_zone(struct zone *zone,
				      unsigned long nr_pages)
{
	unsigned long start_pfn, end_pfn, candidate;
	unsigned long align_pages;
	int ret;

	if (!populated_zone(zone))
		return NULL;

	/*
	 * Align to pageblock boundary (typically 2MB on x86_64) for
	 * alloc_contig_range() to work properly.
	 */
	align_pages = pageblock_nr_pages;

	start_pfn = zone->zone_start_pfn;
	end_pfn = zone_end_pfn(zone);

	if (end_pfn - start_pfn < nr_pages)
		return NULL;

	/* Scan from top down */
	candidate = ALIGN_DOWN(end_pfn - nr_pages, align_pages);

	while (candidate >= start_pfn) {
		if (candidate + nr_pages <= end_pfn &&
		    pfn_valid(candidate) &&
		    pfn_valid(candidate + nr_pages - 1)) {
			ret = alloc_contig_range(candidate,
						 candidate + nr_pages,
						 ACR_FLAGS_CMA,
						 GFP_KERNEL);
			if (ret == 0) {
				pr_info("allocated pfn range [%lx-%lx) from zone %s\n",
					candidate, candidate + nr_pages,
					zone->name);
				return pfn_to_page(candidate);
			}
		}

		if (candidate < align_pages)
			break;
		candidate -= align_pages;
	}

	return NULL;
}

static struct page *lazy_cma_alloc_pages(unsigned long nr_pages, int node)
{
	int nid;
	struct page *pages;
	static const enum zone_type zone_order[] = {
		ZONE_MOVABLE,
		ZONE_NORMAL,
	};
	int i;

	for_each_online_node(nid) {
		pg_data_t *pgdat;

		if (node >= 0 && nid != node)
			continue;

		pgdat = NODE_DATA(nid);

		for (i = 0; i < ARRAY_SIZE(zone_order); i++) {
			struct zone *zone = &pgdat->node_zones[zone_order[i]];

			pages = lazy_cma_try_zone(zone, nr_pages);
			if (pages)
				return pages;
		}
	}

	return NULL;
}

static void lazy_cma_free_buffer(struct lazy_cma_buffer *buffer)
{
	remove_resource(&buffer->iomem);
	free_contig_range(page_to_pfn(buffer->pages), buffer->nr_pages);

	pr_info("freed %lu bytes at %pa\n", buffer->len, &buffer->phys_addr);
	kfree(buffer->name);
	kfree(buffer);
}

static int lazy_cma_allocate(unsigned long len, const char *name, int node,
			     phys_addr_t *out_phys)
{
	struct lazy_cma_buffer *buffer;
	struct page *pages;
	unsigned long nr_pages;

	len = PAGE_ALIGN(len);
	nr_pages = len >> PAGE_SHIFT;

	if (node >= 0 && !node_online(node))
		return -EINVAL;

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	buffer->name = kstrdup(name, GFP_KERNEL);
	if (!buffer->name) {
		kfree(buffer);
		return -ENOMEM;
	}

	pages = lazy_cma_alloc_pages(nr_pages, node);
	if (!pages) {
		pr_err("failed to allocate %lu contiguous pages (%lu MB)\n",
		       nr_pages, len >> 20);
		kfree(buffer->name);
		kfree(buffer);
		return -ENOMEM;
	}

	buffer->len = len;
	buffer->pages = pages;
	buffer->nr_pages = nr_pages;
	buffer->node = node;
	buffer->phys_addr = page_to_phys(pages);

	buffer->iomem.start = buffer->phys_addr;
	buffer->iomem.end = buffer->phys_addr + len - 1;
	buffer->iomem.name = buffer->name;
	buffer->iomem.flags = IORESOURCE_MEM | IORESOURCE_BUSY;
	if (insert_resource(&iomem_resource, &buffer->iomem)) {
		pr_err("could not register iomem region at %pa\n",
		       &buffer->phys_addr);
		free_contig_range(page_to_pfn(pages), nr_pages);
		kfree(buffer->name);
		kfree(buffer);
		return -EBUSY;
	}

	mutex_lock(&lazy_cma_buffers_lock);
	list_add(&buffer->list, &lazy_cma_buffers);
	mutex_unlock(&lazy_cma_buffers_lock);

	*out_phys = buffer->phys_addr;
	pr_info("allocated %lu bytes (%lu MB) at %pa\n",
		len, len >> 20, &buffer->phys_addr);
	return 0;
}

static int lazy_cma_update_iomem(struct lazy_cma_buffer *buffer)
{
	remove_resource(&buffer->iomem);
	buffer->iomem.start = buffer->phys_addr;
	buffer->iomem.end = buffer->phys_addr + buffer->len - 1;
	if (insert_resource(&iomem_resource, &buffer->iomem)) {
		pr_err("could not re-register iomem at %pa\n",
		       &buffer->phys_addr);
		return -EBUSY;
	}
	return 0;
}

static int lazy_cma_grow(struct lazy_cma_buffer *buffer,
			 unsigned long new_nr_pages)
{
	unsigned long old_pfn = page_to_pfn(buffer->pages);
	unsigned long old_nr = buffer->nr_pages;
	unsigned long extend_pfn = old_pfn + old_nr;
	int ret;

	/* Try to extend in-place by allocating adjacent pages */
	ret = alloc_contig_range(extend_pfn, old_pfn + new_nr_pages,
				 ACR_FLAGS_CMA, GFP_KERNEL);
	if (ret == 0) {
		pr_info("grew in-place pfn range [%lx-%lx) -> [%lx-%lx)\n",
			old_pfn, extend_pfn, old_pfn, old_pfn + new_nr_pages);
	} else {
		struct page *new_pages;

		/* Fallback: allocate a new range, free the old one */
		new_pages = lazy_cma_alloc_pages(new_nr_pages, buffer->node);
		if (!new_pages)
			return -ENOMEM;

		free_contig_range(old_pfn, old_nr);

		buffer->pages = new_pages;
		buffer->phys_addr = page_to_phys(new_pages);
		pr_info("grew by reallocation: [%lx-%lx) -> [%lx-%lx)\n",
			old_pfn, old_pfn + old_nr,
			page_to_pfn(new_pages),
			page_to_pfn(new_pages) + new_nr_pages);
	}

	buffer->nr_pages = new_nr_pages;
	buffer->len = new_nr_pages << PAGE_SHIFT;
	return 0;
}

static void lazy_cma_shrink(struct lazy_cma_buffer *buffer,
			    unsigned long new_nr_pages)
{
	unsigned long old_pfn = page_to_pfn(buffer->pages);
	unsigned long tail_pfn = old_pfn + new_nr_pages;
	unsigned long tail_nr = buffer->nr_pages - new_nr_pages;

	free_contig_range(tail_pfn, tail_nr);

	buffer->nr_pages = new_nr_pages;
	buffer->len = new_nr_pages << PAGE_SHIFT;

	pr_info("shrunk pfn range [%lx-%lx) -> [%lx-%lx)\n",
		old_pfn, old_pfn + new_nr_pages + tail_nr,
		old_pfn, tail_pfn);
}

static struct lazy_cma_buffer *lazy_cma_find_buffer(phys_addr_t phys_addr)
{
	struct lazy_cma_buffer *buf;

	list_for_each_entry(buf, &lazy_cma_buffers, list) {
		if (buf->phys_addr == phys_addr)
			return buf;
	}
	return NULL;
}

static long lazy_cma_ioctl_resize(unsigned long arg)
{
	struct lazy_cma_resize_data resize;
	struct lazy_cma_buffer *buffer;
	unsigned long new_len, new_nr_pages;
	int ret;

	if (copy_from_user(&resize, (void __user *)arg, sizeof(resize)))
		return -EFAULT;

	if (!resize.len)
		return -EINVAL;

	new_len = PAGE_ALIGN(resize.len);
	new_nr_pages = new_len >> PAGE_SHIFT;

	mutex_lock(&lazy_cma_buffers_lock);
	buffer = lazy_cma_find_buffer(resize.phys_addr);
	if (!buffer) {
		mutex_unlock(&lazy_cma_buffers_lock);
		return -ENOENT;
	}

	if (new_nr_pages == buffer->nr_pages) {
		ret = 0;
	} else if (new_nr_pages > buffer->nr_pages) {
		ret = lazy_cma_grow(buffer, new_nr_pages);
	} else {
		lazy_cma_shrink(buffer, new_nr_pages);
		ret = 0;
	}

	if (!ret)
		ret = lazy_cma_update_iomem(buffer);
	if (!ret) {
		resize.phys_addr = buffer->phys_addr;
		resize.len = buffer->len;
	}

	mutex_unlock(&lazy_cma_buffers_lock);

	if (ret)
		return ret;

	if (copy_to_user((void __user *)arg, &resize, sizeof(resize)))
		return -EFAULT;

	return 0;
}

static long lazy_cma_ioctl_free(unsigned long arg)
{
	struct lazy_cma_buffer *buffer;
	__u64 phys_addr;

	if (copy_from_user(&phys_addr, (void __user *)arg, sizeof(phys_addr)))
		return -EFAULT;

	mutex_lock(&lazy_cma_buffers_lock);
	buffer = lazy_cma_find_buffer(phys_addr);
	if (!buffer) {
		mutex_unlock(&lazy_cma_buffers_lock);
		return -ENOENT;
	}
	list_del(&buffer->list);
	mutex_unlock(&lazy_cma_buffers_lock);

	lazy_cma_free_buffer(buffer);
	return 0;
}

static long lazy_cma_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct lazy_cma_allocation_data alloc;
	phys_addr_t phys_addr;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (_IOC_NR(cmd) == _IOC_NR(LAZY_CMA_IOCTL_RESIZE))
		return lazy_cma_ioctl_resize(arg);

	if (_IOC_NR(cmd) == _IOC_NR(LAZY_CMA_IOCTL_FREE))
		return lazy_cma_ioctl_free(arg);

	if (_IOC_NR(cmd) != _IOC_NR(LAZY_CMA_IOCTL_ALLOC) ||
	    _IOC_TYPE(cmd) != LAZY_CMA_IOC_MAGIC)
		return -ENOTTY;

	memset(&alloc, 0, sizeof(alloc));
	if (copy_from_user(&alloc, (void __user *)arg,
			   min_t(size_t, _IOC_SIZE(cmd), sizeof(alloc))))
		return -EFAULT;

	if (!alloc.len)
		return -EINVAL;

	/* Ensure name is NUL-terminated; default to "lazy_cma" */
	alloc.name[LAZY_CMA_NAME_MAX - 1] = '\0';
	if (!alloc.name[0])
		strscpy(alloc.name, "lazy_cma", sizeof(alloc.name));

	ret = lazy_cma_allocate(alloc.len, alloc.name, alloc.node, &phys_addr);
	if (ret)
		return ret;

	alloc.phys_addr = phys_addr;
	if (copy_to_user((void __user *)arg, &alloc,
			 min_t(size_t, _IOC_SIZE(cmd), sizeof(alloc))))
		return -EFAULT;

	return 0;
}

static int lazy_cma_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct lazy_cma_buffer *buf;
	phys_addr_t phys = (phys_addr_t)vma->vm_pgoff << PAGE_SHIFT;
	unsigned long size = vma->vm_end - vma->vm_start;

	mutex_lock(&lazy_cma_buffers_lock);
	buf = lazy_cma_find_buffer(phys);
	if (!buf || size > buf->len) {
		mutex_unlock(&lazy_cma_buffers_lock);
		return -EINVAL;
	}
	mutex_unlock(&lazy_cma_buffers_lock);

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
			       size, vma->vm_page_prot);
}

static const struct file_operations lazy_cma_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = lazy_cma_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.mmap = lazy_cma_mmap,
};

static struct miscdevice lazy_cma_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "lazy_cma",
	.fops = &lazy_cma_fops,
	.mode = 0600,
};

static int __init lazy_cma_init(void)
{
	int ret;

	ret = misc_register(&lazy_cma_misc);
	if (ret) {
		pr_err("failed to register misc device: %d\n", ret);
		return ret;
	}

	pr_info("registered /dev/lazy_cma\n");
	return 0;
}

static void __exit lazy_cma_exit(void)
{
	struct lazy_cma_buffer *buf, *tmp;

	mutex_lock(&lazy_cma_buffers_lock);
	list_for_each_entry_safe(buf, tmp, &lazy_cma_buffers, list) {
		list_del(&buf->list);
		lazy_cma_free_buffer(buf);
	}
	mutex_unlock(&lazy_cma_buffers_lock);

	misc_deregister(&lazy_cma_misc);
	pr_info("unloaded\n");
}

module_init(lazy_cma_init);
module_exit(lazy_cma_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Lazy CMA: runtime contiguous memory allocator");
MODULE_AUTHOR("Cong Wang <cwang@multikernel.io>");
