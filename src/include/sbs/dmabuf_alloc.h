#ifndef SBS_DMABUF_ALLOC_H
#define SBS_DMABUF_ALLOC_H

#include "sbs/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum sbs_dmabuf_heap_kind {
    SBS_DMABUF_HEAP_NONE = 0,
    SBS_DMABUF_HEAP_CODECMM,
    SBS_DMABUF_HEAP_GFX,
    SBS_DMABUF_HEAP_LINUX_CMA,
    SBS_DMABUF_HEAP_SYSTEM,
    SBS_DMABUF_HEAP_MEMFD,
    SBS_DMABUF_HEAP_CACHED_CODECMM,
} sbs_dmabuf_heap_kind_t;

typedef enum sbs_dmabuf_sync_direction {
    SBS_DMABUF_SYNC_READ = 1,
    SBS_DMABUF_SYNC_WRITE = 2,
    SBS_DMABUF_SYNC_RW = 3,
} sbs_dmabuf_sync_direction_t;

typedef struct sbs_dmabuf_alloc {
    int codecmm_fd;
    int cached_codecmm_fd;
    int gfx_fd;
    int linux_cma_fd;
    int system_fd;
    bool available;
} sbs_dmabuf_alloc_t;

typedef struct sbs_dmabuf_buffer {
    int fd;
    size_t size;
    sbs_dmabuf_heap_kind_t heap;
} sbs_dmabuf_buffer_t;

int sbs_dmabuf_alloc_open(sbs_dmabuf_alloc_t *alloc);
void sbs_dmabuf_alloc_close(sbs_dmabuf_alloc_t *alloc);
int sbs_dmabuf_alloc_buffer(sbs_dmabuf_alloc_t *alloc,
                            size_t size,
                            uint32_t flags,
                            sbs_dmabuf_buffer_t *buffer);
int sbs_dmabuf_alloc_buffer_from_heap(sbs_dmabuf_alloc_t *alloc,
                                      sbs_dmabuf_heap_kind_t heap,
                                      size_t size,
                                      uint32_t flags,
                                      sbs_dmabuf_buffer_t *buffer);
void *sbs_dmabuf_alloc_map(const sbs_dmabuf_buffer_t *buffer,
                           int prot,
                           size_t length,
                           off_t offset);
int sbs_dmabuf_alloc_sync(const sbs_dmabuf_buffer_t *buffer,
                          bool start,
                          sbs_dmabuf_sync_direction_t dir);

#endif
