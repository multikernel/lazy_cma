// SPDX-License-Identifier: GPL-2.0
/*
 * lazy_cma_tool - Userspace tool for /dev/lazy_cma
 *
 * Usage:
 *   lazy_cma_tool -a <size_MB> [-n name] [-N numa_node]
 *   lazy_cma_tool -r <phys_addr> <new_size_MB>
 *   lazy_cma_tool -f <phys_addr>
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define LAZY_CMA_IOC_MAGIC	'H'
#define LAZY_CMA_NAME_MAX	64

struct lazy_cma_allocation_data {
	__u64 len;
	__u64 phys_addr;
	__s32 node;
	__u32 pad;
	char name[LAZY_CMA_NAME_MAX];
};

struct lazy_cma_resize_data {
	__u64 phys_addr;
	__u64 len;
};

#define LAZY_CMA_IOCTL_ALLOC	_IOWR(LAZY_CMA_IOC_MAGIC, 0x0, \
				      struct lazy_cma_allocation_data)
#define LAZY_CMA_IOCTL_RESIZE	_IOWR(LAZY_CMA_IOC_MAGIC, 0x1, \
				      struct lazy_cma_resize_data)
#define LAZY_CMA_IOCTL_FREE	_IOW(LAZY_CMA_IOC_MAGIC, 0x2, __u64)

#define DEVICE_PATH	"/dev/lazy_cma"

static void usage(const char *prog)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "  %s -a <size_MB> [-n name] [-N numa_node]\n", prog);
	fprintf(stderr, "  %s -r <phys_addr> <new_size_MB>\n", prog);
	fprintf(stderr, "  %s -f <phys_addr>\n", prog);
	exit(1);
}

static int do_alloc(int dev_fd, unsigned long size_mb, const char *name,
		    int node)
{
	struct lazy_cma_allocation_data alloc;

	memset(&alloc, 0, sizeof(alloc));
	alloc.len = (__u64)size_mb * 1024 * 1024;
	alloc.node = node;

	if (name)
		strncpy(alloc.name, name, LAZY_CMA_NAME_MAX - 1);

	if (ioctl(dev_fd, LAZY_CMA_IOCTL_ALLOC, &alloc) < 0) {
		perror("LAZY_CMA_IOCTL_ALLOC");
		return -1;
	}

	printf("allocated %lu MB at 0x%llx\n",
	       size_mb, (unsigned long long)alloc.phys_addr);

	return 0;
}

static int do_resize(int dev_fd, __u64 phys_addr, unsigned long new_size_mb)
{
	struct lazy_cma_resize_data resize;

	memset(&resize, 0, sizeof(resize));
	resize.phys_addr = phys_addr;
	resize.len = (__u64)new_size_mb * 1024 * 1024;

	if (ioctl(dev_fd, LAZY_CMA_IOCTL_RESIZE, &resize) < 0) {
		perror("LAZY_CMA_IOCTL_RESIZE");
		return -1;
	}

	printf("resized to %llu MB at 0x%llx\n",
	       (unsigned long long)(resize.len >> 20),
	       (unsigned long long)resize.phys_addr);

	return 0;
}

static int do_free(int dev_fd, __u64 phys_addr)
{
	if (ioctl(dev_fd, LAZY_CMA_IOCTL_FREE, &phys_addr) < 0) {
		perror("LAZY_CMA_IOCTL_FREE");
		return -1;
	}

	printf("freed allocation at 0x%llx\n", (unsigned long long)phys_addr);
	return 0;
}

int main(int argc, char **argv)
{
	int dev_fd, opt, ret = 0;
	int cmd = 0;
	int node = -1;
	const char *name = NULL;

	while ((opt = getopt(argc, argv, "arfn:N:")) != -1) {
		switch (opt) {
		case 'a':
		case 'r':
		case 'f':
			if (cmd) {
				fprintf(stderr, "only one of -a/-r/-f allowed\n");
				usage(argv[0]);
			}
			cmd = opt;
			break;
		case 'n':
			name = optarg;
			break;
		case 'N':
			node = atoi(optarg);
			break;
		default:
			usage(argv[0]);
		}
	}

	if (!cmd)
		usage(argv[0]);

	dev_fd = open(DEVICE_PATH, O_RDWR);
	if (dev_fd < 0) {
		perror("open " DEVICE_PATH);
		return 1;
	}

	switch (cmd) {
	case 'a':
		if (optind >= argc)
			usage(argv[0]);
		if (!name && optind + 1 < argc)
			name = argv[optind + 1];
		ret = do_alloc(dev_fd, strtoul(argv[optind], NULL, 0), name,
			       node);
		break;
	case 'r':
		if (optind + 1 >= argc)
			usage(argv[0]);
		ret = do_resize(dev_fd, strtoull(argv[optind], NULL, 0),
				strtoul(argv[optind + 1], NULL, 0));
		break;
	case 'f':
		if (optind >= argc)
			usage(argv[0]);
		ret = do_free(dev_fd, strtoull(argv[optind], NULL, 0));
		break;
	}

	close(dev_fd);
	return ret < 0 ? 1 : 0;
}
