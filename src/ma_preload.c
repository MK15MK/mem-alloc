/*
 * Drop-in replacement for the libc allocator:
 *
 *     LD_PRELOAD=./build/libma.so <program>
 *
 * Every allocation entry point must be overridden, otherwise memory from
 * glibc's memalign (say) would later be passed to our free().
 */
#define _GNU_SOURCE
#include "ma.h"

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

void *malloc(size_t n) { return ma_malloc(n); }
void free(void *p) { ma_free(p); }
void *calloc(size_t c, size_t n) { return ma_calloc(c, n); }
void *realloc(void *p, size_t n) { return ma_realloc(p, n); }
void *memalign(size_t a, size_t n) { return ma_memalign(a, n); }
int posix_memalign(void **out, size_t a, size_t n) { return ma_posix_memalign(out, a, n); }
size_t malloc_usable_size(void *p) { return ma_usable_size(p); }

void *aligned_alloc(size_t a, size_t n) { return ma_memalign(a, n); }
void *valloc(size_t n) { return ma_memalign((size_t)sysconf(_SC_PAGESIZE), n); }

void *pvalloc(size_t n)
{
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    if (n > SIZE_MAX - pg) {
        errno = ENOMEM;
        return NULL;
    }
    return ma_memalign(pg, (n + pg - 1) & ~(pg - 1));
}

void *reallocarray(void *p, size_t c, size_t n)
{
    size_t total;
    if (__builtin_mul_overflow(c, n, &total)) {
        errno = ENOMEM;
        return NULL;
    }
    return ma_realloc(p, total);
}
