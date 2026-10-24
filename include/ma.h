/*
 * ma - a small general-purpose memory allocator.
 *
 * Segregated free lists + boundary-tag coalescing on top of sbrk/mmap,
 * with per-arena mutexes for multithreaded use. See README.md for design.
 */
#ifndef MA_H
#define MA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void  *ma_malloc(size_t n);
void   ma_free(void *p);
void  *ma_calloc(size_t count, size_t n);
void  *ma_realloc(void *p, size_t n);
void  *ma_memalign(size_t align, size_t n);
int    ma_posix_memalign(void **out, size_t align, size_t n);
size_t ma_usable_size(void *p);

typedef struct ma_stats {
    size_t os_bytes;       /* bytes currently obtained from the OS (heap + mmap) */
    size_t peak_os_bytes;  /* high-water mark of os_bytes */
    size_t heap_bytes;     /* bytes in sbrk/mmap heap segments */
    size_t inuse_bytes;    /* bytes in allocated heap chunks (incl. headers) */
    size_t free_bytes;     /* bytes in free heap chunks (bins + top) */
    size_t mmap_bytes;     /* bytes in directly mmapped large chunks */
    size_t mmap_count;     /* number of directly mmapped large chunks */
    unsigned narenas;      /* arenas in use */
} ma_stats_t;

void ma_get_stats(ma_stats_t *st);

/* Walk every arena and verify all heap invariants. Returns 0 if the heap
 * is consistent, otherwise prints a diagnostic to stderr and returns -1. */
int  ma_check(void);

/* Print arenas, segments and chunks to the given file descriptor. */
void ma_dump(int fd);

#ifdef __cplusplus
}
#endif

#endif /* MA_H */
