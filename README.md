# Lazy CMA

Runtime contiguous memory allocator for Linux. Like CMA, but without boot-time reservation.

## Why not CMA?

CMA requires you to decide the contiguous memory size **before** the system is running:

| Method | When | Resizable | NUMA-aware | Hotplug memory | Flexibility |
|--------|------|-----------|------------|----------------|-------------|
| `CONFIG_CMA_SIZE_MBYTES` | Kernel compile time | No | No | No | Requires kernel rebuild |
| `cma=256M` boot param | Boot time | No | No | No | Requires reboot |
| `numa_cma=<node>:<size>` | Boot time | No | Yes | No | Requires reboot |
| **lazy_cma** | **Runtime** | **Yes** | **Yes** | **Yes** | **No planning needed** |

CMA reserves a region at boot where only movable pages are placed, guaranteeing
that contiguous allocation will succeed. lazy_cma instead uses `alloc_contig_range()`
to migrate pages out of any zone on demand. No reservation, no reboot, no kernel
rebuild.

The tradeoff: CMA guarantees success (reserved region), lazy_cma is best-effort
(may fail on heavily fragmented systems). In practice, lazy_cma works well on
systems when still have sufficient free memory, e.g. right after booting.

Mainline CMA does support per-NUMA reservations via `numa_cma=<node>:<size>` and
`cma_pernuma=<size>` (`CONFIG_DMA_NUMA_CMA`), but these regions must be reserved
at boot from nodes that are online during early boot. In typical CXL and PMEM
deployments, memory is marked as Specific Purpose (`EFI_MEMORY_SP`) by firmware
and deferred to driver management; it is onlined post-boot via `dax/kmem` and
cannot participate in CMA. lazy_cma uses `alloc_contig_range()` at runtime and
works with any memory that is online in a zone, with NUMA node selection (`-N`)
and no boot-time planning.

Additionally, neither CMA nor its dma-buf heap (`cma_heap`) expose allocation
physical addresses. lazy_cma registers each allocation in `/proc/iomem`, making
the physical location visible for debugging and for use cases like multikernel
where the physical address matters.

## Usage

```bash
insmod lazy_cma.ko          # creates /dev/lazy_cma
```

### Allocate

```bash
# Allocate 256 MB
lazy_cma_tool -a 256

# Allocate 256 MB with custom /proc/iomem name (e.g. to fake crashkernel=)
lazy_cma_tool -a 256 -n "Crash kernel"

# Allocate 256 MB from NUMA node 2 (e.g. CXL memory)
lazy_cma_tool -a 256 -N 2
```

### Resize

```bash
# Grow to 512 MB (tries in-place first, then reallocates)
lazy_cma_tool -r 0x100000000 512

# Shrink to 128 MB
lazy_cma_tool -r 0x100000000 128
```

### Free

```bash
lazy_cma_tool -f 0x100000000
```

### C API

```c
int fd = open("/dev/lazy_cma", O_RDWR);

/* Allocate */
struct lazy_cma_allocation_data alloc = {
    .len = 256 * 1024 * 1024,
    .node = -1,  /* -1 = any node, or specify e.g. 2 for CXL */
};
strncpy(alloc.name, "Crash kernel", LAZY_CMA_NAME_MAX - 1);
ioctl(fd, LAZY_CMA_IOCTL_ALLOC, &alloc);
printf("phys_addr: 0x%llx\n", alloc.phys_addr);

/* Resize */
struct lazy_cma_resize_data resize = {
    .phys_addr = alloc.phys_addr,
    .len = 512 * 1024 * 1024,
};
ioctl(fd, LAZY_CMA_IOCTL_RESIZE, &resize);

/* Free */
__u64 addr = resize.phys_addr;
ioctl(fd, LAZY_CMA_IOCTL_FREE, &addr);
```

Allocated regions are visible in `/proc/iomem`:

```
100000000-10fffffff : Crash kernel
```

Buffers persist across processes and are identified by physical address.
They are freed explicitly via `LAZY_CMA_IOCTL_FREE` or on module unload.

## Building

```bash
make
```

## License

GPL-2.0
