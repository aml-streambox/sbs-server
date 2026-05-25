#define SBS_LOG_COMP "dmabuf"

#include "sbs/dmabuf_alloc.h"
#include "sbs/log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/memfd.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static int sbs_memfd_create(const char *name, unsigned int flags)
{
#ifdef SYS_memfd_create
    return (int)syscall(SYS_memfd_create, name, flags);
#else
    errno = ENOSYS;
    return -1;
#endif
}

static const char *heap_name(sbs_dmabuf_heap_kind_t heap)
{
    switch (heap) {
    case SBS_DMABUF_HEAP_CODECMM: return "codecmm";
    case SBS_DMABUF_HEAP_CACHED_CODECMM: return "cached_codecmm";
    case SBS_DMABUF_HEAP_GFX:     return "gfx";
    case SBS_DMABUF_HEAP_LINUX_CMA: return "linux_cma";
    case SBS_DMABUF_HEAP_SYSTEM:  return "system";
    case SBS_DMABUF_HEAP_MEMFD:   return "memfd";
    default:                      return "unknown";
    }
}

static int open_heap(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0 && errno != ENOENT)
        LOG_W("open(%s) failed: %s", path, strerror(errno));
    return fd;
}

int sbs_dmabuf_alloc_open(sbs_dmabuf_alloc_t *alloc)
{
    if (!alloc)
        return SBS_ERR_INVAL;

    memset(alloc, 0, sizeof(*alloc));
    alloc->codecmm_fd = -1;
    alloc->cached_codecmm_fd = -1;
    alloc->gfx_fd = -1;
    alloc->linux_cma_fd = -1;
    alloc->system_fd = -1;
    alloc->codecmm_fd = open_heap("/dev/dma_heap/heap-codecmm");
    alloc->cached_codecmm_fd = open_heap("/dev/dma_heap/heap-cached-codecmm");
    alloc->gfx_fd = open_heap("/dev/dma_heap/heap-gfx");
    alloc->linux_cma_fd = open_heap("/dev/dma_heap/linux,cma");
    alloc->system_fd = open_heap("/dev/dma_heap/system");
    alloc->available = alloc->codecmm_fd >= 0 || alloc->cached_codecmm_fd >= 0 ||
                       alloc->gfx_fd >= 0 ||
                       alloc->linux_cma_fd >= 0 || alloc->system_fd >= 0;
    return SBS_OK;
}

void sbs_dmabuf_alloc_close(sbs_dmabuf_alloc_t *alloc)
{
    if (!alloc)
        return;
    if (alloc->codecmm_fd >= 0)
        close(alloc->codecmm_fd);
    if (alloc->cached_codecmm_fd >= 0)
        close(alloc->cached_codecmm_fd);
    if (alloc->gfx_fd >= 0)
        close(alloc->gfx_fd);
    if (alloc->linux_cma_fd >= 0)
        close(alloc->linux_cma_fd);
    if (alloc->system_fd >= 0)
        close(alloc->system_fd);
    alloc->codecmm_fd = -1;
    alloc->cached_codecmm_fd = -1;
    alloc->gfx_fd = -1;
    alloc->linux_cma_fd = -1;
    alloc->system_fd = -1;
    alloc->available = false;
}

static int alloc_from_heap(int heap_fd, size_t size, uint32_t flags)
{
    struct dma_heap_allocation_data data = {
        .len = size,
        .fd_flags = O_CLOEXEC | O_RDWR,
        .heap_flags = flags,
    };
    int fd;
    if (heap_fd < 0)
        return -1;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) != 0)
        return -1;
    fd = (int)data.fd;
    /* Guard against fd 0/1/2 being returned (indicates stdio was closed).
       Dup to a safe fd to avoid passing stdin/stdout/stderr to kernel
       ioctls that expect DMA-BUF fds. */
    if (fd >= 0 && fd <= 2) {
        int safe_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        LOG_W("DMA-BUF alloc got low fd=%d, dup'd to %d", fd, safe_fd);
        close(fd);
        fd = safe_fd;
    }
    return fd;
}

int sbs_dmabuf_alloc_buffer(sbs_dmabuf_alloc_t *alloc,
                            size_t size,
                            uint32_t flags,
                            sbs_dmabuf_buffer_t *buffer)
{
    int fd = -1;

    if (!alloc || !buffer || size == 0)
        return SBS_ERR_INVAL;

    memset(buffer, 0, sizeof(*buffer));
    buffer->fd = -1;

    fd = alloc_from_heap(alloc->codecmm_fd, size, flags);
    if (fd >= 0) {
        buffer->fd = fd;
        buffer->size = size;
        buffer->heap = SBS_DMABUF_HEAP_CODECMM;
        LOG_I("DMA-BUF allocated %zu bytes from %s heap", size, heap_name(buffer->heap));
        return SBS_OK;
    }

    fd = alloc_from_heap(alloc->cached_codecmm_fd, size, flags);
    if (fd >= 0) {
        buffer->fd = fd;
        buffer->size = size;
        buffer->heap = SBS_DMABUF_HEAP_CACHED_CODECMM;
        LOG_I("DMA-BUF allocated %zu bytes from %s heap", size, heap_name(buffer->heap));
        return SBS_OK;
    }

    fd = alloc_from_heap(alloc->gfx_fd, size, flags);
    if (fd >= 0) {
        buffer->fd = fd;
        buffer->size = size;
        buffer->heap = SBS_DMABUF_HEAP_GFX;
        LOG_I("DMA-BUF allocated %zu bytes from %s heap", size, heap_name(buffer->heap));
        return SBS_OK;
    }

    fd = alloc_from_heap(alloc->linux_cma_fd, size, flags);
    if (fd >= 0) {
        buffer->fd = fd;
        buffer->size = size;
        buffer->heap = SBS_DMABUF_HEAP_LINUX_CMA;
        LOG_I("DMA-BUF allocated %zu bytes from %s heap", size, heap_name(buffer->heap));
        return SBS_OK;
    }

    fd = alloc_from_heap(alloc->system_fd, size, flags);
    if (fd >= 0) {
        buffer->fd = fd;
        buffer->size = size;
        buffer->heap = SBS_DMABUF_HEAP_SYSTEM;
        LOG_I("DMA-BUF allocated %zu bytes from %s heap", size, heap_name(buffer->heap));
        return SBS_OK;
    }

    fd = sbs_memfd_create("sbs-dmabuf-fallback", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return SBS_ERR_IO;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return SBS_ERR_IO;
    }

    buffer->fd = fd;
    buffer->size = size;
    buffer->heap = SBS_DMABUF_HEAP_MEMFD;
    return SBS_OK;
}

int sbs_dmabuf_alloc_buffer_from_heap(sbs_dmabuf_alloc_t *alloc,
                                      sbs_dmabuf_heap_kind_t heap,
                                      size_t size,
                                      uint32_t flags,
                                      sbs_dmabuf_buffer_t *buffer)
{
    int heap_fd = -1;
    int fd;

    if (!alloc || !buffer || size == 0)
        return SBS_ERR_INVAL;

    switch (heap) {
    case SBS_DMABUF_HEAP_CODECMM: heap_fd = alloc->codecmm_fd; break;
    case SBS_DMABUF_HEAP_CACHED_CODECMM: heap_fd = alloc->cached_codecmm_fd; break;
    case SBS_DMABUF_HEAP_GFX: heap_fd = alloc->gfx_fd; break;
    case SBS_DMABUF_HEAP_LINUX_CMA: heap_fd = alloc->linux_cma_fd; break;
    case SBS_DMABUF_HEAP_SYSTEM: heap_fd = alloc->system_fd; break;
    default: return SBS_ERR_INVAL;
    }

    memset(buffer, 0, sizeof(*buffer));
    buffer->fd = -1;
    fd = alloc_from_heap(heap_fd, size, flags);
    if (fd < 0)
        return SBS_ERR_IO;

    buffer->fd = fd;
    buffer->size = size;
    buffer->heap = heap;
    LOG_I("DMA-BUF allocated %zu bytes from %s heap", size, heap_name(buffer->heap));
    return SBS_OK;
}

void *sbs_dmabuf_alloc_map(const sbs_dmabuf_buffer_t *buffer,
                           int prot,
                           size_t length,
                           off_t offset)
{
    if (!buffer || buffer->fd < 0)
        return MAP_FAILED;
    return mmap(NULL, length ? length : buffer->size, prot, MAP_SHARED, buffer->fd, offset);
}

int sbs_dmabuf_alloc_sync(const sbs_dmabuf_buffer_t *buffer,
                          bool start,
                          sbs_dmabuf_sync_direction_t dir)
{
    struct dma_buf_sync sync = {
        .flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END),
    };

    if (!buffer || buffer->fd < 0)
        return SBS_ERR_INVAL;

    switch (dir) {
    case SBS_DMABUF_SYNC_READ:
        sync.flags |= DMA_BUF_SYNC_READ;
        break;
    case SBS_DMABUF_SYNC_WRITE:
        sync.flags |= DMA_BUF_SYNC_WRITE;
        break;
    case SBS_DMABUF_SYNC_RW:
    default:
        sync.flags |= DMA_BUF_SYNC_RW;
        break;
    }

    if (buffer->heap == SBS_DMABUF_HEAP_MEMFD)
        return SBS_OK;

    return ioctl(buffer->fd, DMA_BUF_IOCTL_SYNC, &sync) == 0 ? SBS_OK : SBS_ERR_IO;
}
