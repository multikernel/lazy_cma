// SPDX-License-Identifier: GPL-2.0
/*
 * lazy_kdump - Set crashk_res to match an existing "Crash kernel" iomem region
 *
 * This module sets the kernel's internal crashk_res to match a region
 * (e.g. created by lazy_cma), so that kexec -p can load a crash kernel
 * without the crashkernel= boot parameter.
 *
 * Usage:
 *   # Helper resolves crashk_res address and crash region automatically:
 *   ADDR=$(sudo awk '/crashk_res/{print "0x"$1}' /proc/kallsyms)
 *   RANGE=$(sudo awk '/Crash kernel/{print $1}' /proc/iomem)
 *   sudo insmod lazy_kdump.ko crashk_res_addr=$ADDR \
 *       start=0x${RANGE%-*} end=0x${RANGE#*-}
 *
 * Copyright (C) 2026 Multikernel Technologies, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "lazy_kdump: " fmt

#include <linux/module.h>
#include <linux/ioport.h>

static unsigned long crashk_res_addr;
static unsigned long long start;
static unsigned long long end;
module_param(crashk_res_addr, ulong, 0444);
module_param(start, ullong, 0444);
module_param(end, ullong, 0444);

static struct resource *crashk_res_ptr;
static resource_size_t orig_start;
static resource_size_t orig_end;

static int __init lazy_kdump_init(void)
{
	if (!crashk_res_addr) {
		pr_err("crashk_res_addr not set\n");
		return -EINVAL;
	}

	if (!start || !end || start >= end) {
		pr_err("invalid range: 0x%llx-0x%llx\n", start, end);
		return -EINVAL;
	}

	crashk_res_ptr = (struct resource *)crashk_res_addr;

	/* Save original values for restore on unload */
	orig_start = crashk_res_ptr->start;
	orig_end = crashk_res_ptr->end;

	crashk_res_ptr->start = start;
	crashk_res_ptr->end = end;

	pr_info("set crashk_res to 0x%llx-0x%llx (%llu MB)\n",
		start, end, (end - start + 1) >> 20);
	return 0;
}

static void __exit lazy_kdump_exit(void)
{
	crashk_res_ptr->start = orig_start;
	crashk_res_ptr->end = orig_end;
	pr_info("restored crashk_res\n");
}

module_init(lazy_kdump_init);
module_exit(lazy_kdump_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Lazy kdump: set crashk_res from lazy_cma allocation");
MODULE_AUTHOR("Cong Wang <cwang@multikernel.io>");
