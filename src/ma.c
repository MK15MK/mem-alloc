/*
 * ma.c - segregated-fit allocator with boundary-tag coalescing.
 *
 * Chunk layout (dlmalloc/ptmalloc style, 16-byte aligned):
 *
 *     chunk -> +-----------------------------+
 *              | prev_size (valid if prev    |  only meaningful when the
 *              |   chunk is free: footer)    |  previous chunk is free
 *              +-----------------------------+
 *              | head = size | flags | arena |
 *     mem   -> +-----------------------------+
 *              | fd  (free chunks only)      |
 *              | bk  (free chunks only)      |
 *              | ... payload ...             |
 *     next  -> +-----------------------------+
 *              | prev_size  <- footer of this chunk when free,
 *              |              last 8 payload bytes when in use
 *
 * Every heap segment starts with a segment header and ends with a zero-size
 * in-use "fence" chunk, so coalescing never crosses segment boundaries and
 * sbrk regions interleaved with foreign brk users are handled safely.
 *
 * Invariants (verified by ma_check):
 *   - no two free chunks are adjacent (eager coalescing);
 *   - a free chunk's footer equals its size and its successor's PREV_INUSE
 *     bit is clear;
 *   - every free chunk except the arena's top chunk is in exactly the bin
 *     bin_index(size), and binmap mirrors which bins are non-empty;
 *   - the top chunk is the last chunk of the arena's current segment.
 */
#define _GNU_SOURCE
#include "ma.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef MA_VALGRIND
#include <valgrind/memcheck.h>
#include <valgrind/valgrind.h>
/* All memory we get from the OS is marked NOACCESS; only live user blocks
 * (described with MALLOCLIKE/FREELIKE) are addressable, so memcheck flags
 * overflows into chunk slack and use after free. Allocator metadata thus
 * always lives in NOACCESS memory: error reporting is suspended while the
 * allocator runs, and fully-unaddressable loads read as defined. (Marking
 * whole regions matters: with --partial-loads-ok=yes, a word that is only
 * partly addressable would load as undefined.) */
#define VG_ENTER()          VALGRIND_DISABLE_ERROR_REPORTING
#define VG_LEAVE()          VALGRIND_ENABLE_ERROR_REPORTING
#define VG_ALLOC(p, n, z)   VALGRIND_MALLOCLIKE_BLOCK((p), (n), 0, (z))
#define VG_FREE(p)          VALGRIND_FREELIKE_BLOCK((p), 0)
#define VG_NOACCESS(p, n)   VALGRIND_MAKE_MEM_NOACCESS((p), (n))
#else
#define VG_ENTER()          ((void)0)
#define VG_LEAVE()          ((void)0)
#define VG_ALLOC(p, n, z)   ((void)0)
#define VG_FREE(p)          ((void)0)
#define VG_NOACCESS(p, n)   ((void)0)
#endif

/* ------------------------------------------------------------------------ */
/* Tunables                                                                 */
/* ------------------------------------------------------------------------ */

#define MA_ALIGN        16
#define MA_MAX_ARENAS   64
#define MMAP_THRESHOLD  (128 * 1024)    /* requests >= this go to mmap      */
#define SBRK_MIN_GROW   (128 * 1024)    /* minimum sbrk increment           */
#define SEG_MIN_SIZE    (1024 * 1024)   /* minimum mmap'd heap segment      */
#define TRIM_THRESHOLD  (256 * 1024)    /* trim top when it exceeds this... */
#define TOP_KEEP        (128 * 1024)    /* ...keeping this much             */
#define LARGE_SCAN_MAX  32              /* best-fit scan limit per bin      */
#define TCACHE_COUNT    8               /* cached chunks per size class     */

/* ------------------------------------------------------------------------ */
/* Chunks                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct chunk {
    size_t prev_size;
    size_t head;
    struct chunk *fd;
    struct chunk *bk;
} chunk_t;

#define HDR_SIZE     (2 * sizeof(size_t))
#define MIN_CHUNK    sizeof(chunk_t)
#define FENCE_SIZE   HDR_SIZE

#define F_INUSE      ((size_t)1)
#define F_PREV_INUSE ((size_t)2)
#define F_MMAPPED    ((size_t)4)
#define FLAG_MASK    ((size_t)0xF)
#define ARENA_SHIFT  56
#define SIZE_MASK    ((((size_t)1 << ARENA_SHIFT) - 1) & ~FLAG_MASK)
#define MAX_REQUEST  ((size_t)1 << 47)

static inline size_t csize(const chunk_t *c) { return c->head & SIZE_MASK; }
static inline int c_inuse(const chunk_t *c) { return (c->head & F_INUSE) != 0; }
static inline int c_prev_inuse(const chunk_t *c) { return (c->head & F_PREV_INUSE) != 0; }
static inline int c_mmapped(const chunk_t *c) { return (c->head & F_MMAPPED) != 0; }
static inline unsigned c_arena(const chunk_t *c) { return (unsigned)(c->head >> ARENA_SHIFT); }

static inline chunk_t *c_at(void *p, size_t off) { return (chunk_t *)((char *)p + off); }
static inline chunk_t *c_next(chunk_t *c) { return c_at(c, csize(c)); }
static inline chunk_t *c_prev(chunk_t *c) { return (chunk_t *)((char *)c - c->prev_size); }
static inline void *c2mem(chunk_t *c) { return (char *)c + HDR_SIZE; }
static inline chunk_t *mem2c(void *p) { return (chunk_t *)((char *)p - HDR_SIZE); }

static inline void set_head(chunk_t *c, size_t size, size_t flags, unsigned arena)
{
    c->head = size | flags | ((size_t)arena << ARENA_SHIFT);
}

static inline uintptr_t align_up(uintptr_t x, size_t a) { return (x + a - 1) & ~(uintptr_t)(a - 1); }
static inline uintptr_t align_down(uintptr_t x, size_t a) { return x & ~(uintptr_t)(a - 1); }

/* Chunk size for an n-byte request: payload may spill into the next
 * chunk's prev_size field, so only one word of overhead per chunk. */
static inline int req2size(size_t n, size_t *out)
{
    if (n >= MAX_REQUEST)
        return -1;
    size_t s = align_up(n + sizeof(size_t), MA_ALIGN);
    *out = s < MIN_CHUNK ? MIN_CHUNK : s;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Bins: 62 exact-size small bins (32..1008 step 16), then 4 bins per power */
/* of two for large chunks.                                                 */
/* ------------------------------------------------------------------------ */

#define NSMALL        62
#define SMALL_LIMIT   1024
#define LARGE_LOG_MIN 10
#define LARGE_LOG_MAX 40
#define NBINS         (NSMALL + 4 * (LARGE_LOG_MAX - LARGE_LOG_MIN + 1))
#define NMAP_WORDS    ((NBINS + 63) / 64)

static inline unsigned bin_index(size_t sz)
{
    if (sz < SMALL_LIMIT)
        return (unsigned)(sz >> 4) - 2;
    unsigned lg = 63u - (unsigned)__builtin_clzl(sz);
    if (lg > LARGE_LOG_MAX)
        return NBINS - 1;
    unsigned sub = (unsigned)(sz >> (lg - 2)) & 3u;
    return NSMALL + (lg - LARGE_LOG_MIN) * 4 + sub;
}

/* ------------------------------------------------------------------------ */
/* Segments and arenas                                                      */
/* ------------------------------------------------------------------------ */

typedef struct segment {
    struct segment *next;
    char *base;         /* start of the OS region */
    size_t size;        /* length of the OS region */
    int from_sbrk;
} segment_t;

#define SEG_HDR_SIZE  align_up(sizeof(segment_t), MA_ALIGN)

typedef struct arena {
    pthread_mutex_t lock;
    unsigned id;
    chunk_t *top;           /* wilderness chunk at end of segs (current) */
    segment_t *segs;        /* current segment first */
    size_t inuse_bytes;
    size_t heap_bytes;
    uint64_t binmap[NMAP_WORDS];
    chunk_t *bins[NBINS];
} __attribute__((aligned(64))) arena_t;

static arena_t arenas[MA_MAX_ARENAS];
static unsigned narenas = 1;
static atomic_uint next_arena;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static size_t page_size = 4096;
static int sbrk_disabled;

static atomic_size_t g_os_bytes, g_peak_os_bytes, g_mmap_bytes, g_mmap_count;

static __thread arena_t *tls_arena __attribute__((tls_model("initial-exec")));

/* Per-thread cache of small chunks (32..1008 bytes), one LIFO list per small
 * bin, consulted before any arena lock is taken. Cached chunks stay marked
 * in use, so to the arenas (and ma_check) they look allocated. A cached
 * chunk's bk word holds tcache_cookie so double frees can be detected. */
enum { TC_UNINIT, TC_ON, TC_OFF };

typedef struct tcache {
    int state;
    uint8_t count[NSMALL];
    chunk_t *head[NSMALL];
} tcache_t;

static __thread tcache_t tcache __attribute__((tls_model("initial-exec")));
static int tcache_enabled = 1;
static pthread_key_t tcache_key;
static uintptr_t tcache_cookie;

static void tcache_thread_exit(void *arg);

/* ------------------------------------------------------------------------ */
/* Diagnostics (no stdio: must not allocate while holding arena locks)      */
/* ------------------------------------------------------------------------ */

static void ma_log(int fd, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (len > (int)sizeof buf - 1)
        len = (int)sizeof buf - 1;
    if (len > 0 && write(fd, buf, (size_t)len) < 0)
        return;
}

static void corruption(const char *what, void *p)
{
    ma_log(2, "ma: %s (%p)\n", what, p);
    abort();
}

/* ------------------------------------------------------------------------ */
/* OS accounting                                                            */
/* ------------------------------------------------------------------------ */

static void os_add(size_t n)
{
    size_t now = atomic_fetch_add_explicit(&g_os_bytes, n, memory_order_relaxed) + n;
    size_t peak = atomic_load_explicit(&g_peak_os_bytes, memory_order_relaxed);
    while (now > peak &&
           !atomic_compare_exchange_weak_explicit(&g_peak_os_bytes, &peak, now,
                                                  memory_order_relaxed, memory_order_relaxed))
        ;
}

static void os_sub(size_t n)
{
    atomic_fetch_sub_explicit(&g_os_bytes, n, memory_order_relaxed);
}

/* ------------------------------------------------------------------------ */
/* Initialisation and fork safety                                           */
/* ------------------------------------------------------------------------ */

static void atfork_prepare(void)
{
    for (unsigned i = 0; i < narenas; i++)
        pthread_mutex_lock(&arenas[i].lock);
}

static void atfork_release(void)
{
    for (unsigned i = narenas; i-- > 0;)
        pthread_mutex_unlock(&arenas[i].lock);
}

static void ma_init(void)
{
    long pg = sysconf(_SC_PAGESIZE);
    if (pg > 0)
        page_size = (size_t)pg;

    /* sched_getaffinity is a plain syscall; sysconf(_SC_NPROCESSORS_*) may
     * allocate in some libc versions, which would recurse into us. */
    cpu_set_t set;
    unsigned ncpu = 1;
    if (sched_getaffinity(0, sizeof set, &set) == 0)
        ncpu = (unsigned)CPU_COUNT(&set);
    narenas = ncpu < 1 ? 1 : ncpu > MA_MAX_ARENAS ? MA_MAX_ARENAS : ncpu;

    const char *env = getenv("MA_ARENAS");
    if (env) {
        long v = strtol(env, NULL, 10);
        if (v >= 1 && v <= MA_MAX_ARENAS)
            narenas = (unsigned)v;
    }
    if (getenv("MA_NO_SBRK"))
        sbrk_disabled = 1;

    for (unsigned i = 0; i < MA_MAX_ARENAS; i++) {
        pthread_mutex_init(&arenas[i].lock, NULL);
        arenas[i].id = i;
    }
    pthread_atfork(atfork_prepare, atfork_release, atfork_release);

    env = getenv("MA_TCACHE");
    if (env && env[0] == '0')
        tcache_enabled = 0;
    if (pthread_key_create(&tcache_key, tcache_thread_exit) != 0)
        tcache_enabled = 0;
    tcache_cookie = ((uintptr_t)&tcache_key ^ ((uintptr_t)getpid() << 32)) * 0x9E3779B97F4A7C15ull | 1;
}

/* Lock the calling thread's arena. If it is contended, try the others and
 * migrate to the first free one, so threads spread out under contention. */
static arena_t *lock_my_arena(void)
{
    arena_t *a = tls_arena;
    if (__builtin_expect(a == NULL, 0)) {
        pthread_once(&init_once, ma_init);
        a = &arenas[atomic_fetch_add(&next_arena, 1) % narenas];
        tls_arena = a;
    }
    if (pthread_mutex_trylock(&a->lock) == 0)
        return a;
    for (unsigned k = 1; k < narenas; k++) {
        arena_t *b = &arenas[(a->id + k) % narenas];
        if (pthread_mutex_trylock(&b->lock) == 0) {
            tls_arena = b;
            return b;
        }
    }
    pthread_mutex_lock(&a->lock);
    return a;
}

/* ------------------------------------------------------------------------ */
/* Free lists                                                               */
/* ------------------------------------------------------------------------ */

static inline void bin_insert(arena_t *a, chunk_t *c)
{
    unsigned i = bin_index(csize(c));
    chunk_t *h = a->bins[i];
    c->fd = h;
    c->bk = NULL;
    if (h)
        h->bk = c;
    a->bins[i] = c;
    a->binmap[i >> 6] |= (uint64_t)1 << (i & 63);
}

static inline void bin_unlink(arena_t *a, chunk_t *c)
{
    if (c->bk) {
        c->bk->fd = c->fd;
    } else {
        unsigned i = bin_index(csize(c));
        a->bins[i] = c->fd;
        if (!c->fd)
            a->binmap[i >> 6] &= ~((uint64_t)1 << (i & 63));
    }
    if (c->fd)
        c->fd->bk = c->bk;
}

/* First non-empty bin with index >= i, or -1. */
static inline int next_bin(const arena_t *a, unsigned i)
{
    for (unsigned w = i >> 6; w < NMAP_WORDS; w++) {
        uint64_t m = a->binmap[w];
        if (w == (i >> 6))
            m &= ~(uint64_t)0 << (i & 63);
        if (m)
            return (int)(w * 64 + (unsigned)__builtin_ctzll(m));
    }
    return -1;
}

/* Turn c into a free chunk of sz bytes: write head and footer and clear the
 * successor's PREV_INUSE bit. The predecessor is always in use. */
static inline void mark_free(arena_t *a, chunk_t *c, size_t sz)
{
    set_head(c, sz, F_PREV_INUSE, a->id);
    chunk_t *n = c_at(c, sz);
    n->prev_size = sz;
    n->head &= ~F_PREV_INUSE;
}

/* ------------------------------------------------------------------------ */
/* Segments, top chunk, growing and trimming                                */
/* ------------------------------------------------------------------------ */

static inline chunk_t *seg_fence(const segment_t *s)
{
    return (chunk_t *)(align_down((uintptr_t)(s->base + s->size), MA_ALIGN) - FENCE_SIZE);
}

static inline chunk_t *seg_first(const segment_t *s)
{
    return (chunk_t *)(align_up((uintptr_t)s->base, MA_ALIGN) + SEG_HDR_SIZE);
}

/* Make the region [raw, raw+len) the arena's current segment; its whole
 * body becomes the new top chunk. The old top is retired into the bins. */
static void add_segment(arena_t *a, char *raw, size_t len, int from_sbrk)
{
    VG_NOACCESS(raw, len);
    segment_t *s = (segment_t *)align_up((uintptr_t)raw, MA_ALIGN);
    s->base = raw;
    s->size = len;
    s->from_sbrk = from_sbrk;
    s->next = a->segs;

    if (a->top)
        bin_insert(a, a->top);

    chunk_t *first = seg_first(s);
    chunk_t *fence = seg_fence(s);
    size_t sz = (size_t)((char *)fence - (char *)first);
    set_head(first, sz, F_PREV_INUSE, a->id);
    fence->prev_size = sz;
    set_head(fence, 0, F_INUSE, a->id);

    a->segs = s;
    a->top = first;
    a->heap_bytes += len;
    os_add(len);
}

/* Ensure the top chunk can supply nb bytes and still leave a MIN_CHUNK. */
static int grow_arena(arena_t *a, size_t nb)
{
    size_t need = nb + MIN_CHUNK + SEG_HDR_SIZE + FENCE_SIZE + 2 * MA_ALIGN;

    if (a->id == 0 && !sbrk_disabled) {
        size_t incr = align_up(need < SBRK_MIN_GROW ? SBRK_MIN_GROW : need, page_size);
        segment_t *s = a->segs;
        char *p = sbrk((intptr_t)incr);
        if (p != (char *)-1) {
            if (s && s->from_sbrk && p == s->base + s->size) {
                /* Contiguous: slide the fence out and enlarge top. */
                VG_NOACCESS(p, incr);
                chunk_t *t = a->top;
                s->size += incr;
                a->heap_bytes += incr;
                os_add(incr);
                chunk_t *fence = seg_fence(s);
                size_t ts = (size_t)((char *)fence - (char *)t);
                set_head(t, ts, F_PREV_INUSE, a->id);
                fence->prev_size = ts;
                set_head(fence, 0, F_INUSE, a->id);
            } else {
                add_segment(a, p, incr, 1);
            }
            return 0;
        }
        sbrk_disabled = 1;   /* brk exhausted or unavailable: use mmap */
    }

    size_t len = align_up(need < SEG_MIN_SIZE ? SEG_MIN_SIZE : need, page_size);
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    add_segment(a, p, len, 0);
    return 0;
}

/* Give memory at the end of the current segment back to the OS when the top
 * chunk is large. Works for sbrk (if we still own the break) and mmap. */
static void maybe_trim(arena_t *a)
{
    chunk_t *t = a->top;
    size_t ts = csize(t);
    if (ts < TRIM_THRESHOLD)
        return;
    size_t release = align_down(ts - TOP_KEEP, page_size);
    if (release == 0)
        return;
    segment_t *s = a->segs;
    char *end = s->base + s->size;
    if (s->from_sbrk) {
        if (sbrk(0) != end || sbrk(-(intptr_t)release) == (void *)-1)
            return;
    } else if (munmap(end - release, release) != 0) {
        return;
    }
    s->size -= release;
    a->heap_bytes -= release;
    os_sub(release);
    chunk_t *fence = seg_fence(s);
    size_t nts = (size_t)((char *)fence - (char *)t);
    set_head(t, nts, F_PREV_INUSE, a->id);
    fence->prev_size = nts;
    set_head(fence, 0, F_INUSE, a->id);
}

/* c is a free chunk followed by a fence. If it is also the first chunk of a
 * non-current mmap segment, the whole segment is empty: unmap it. */
static int release_segment(arena_t *a, chunk_t *c)
{
    for (segment_t **pp = &a->segs->next; *pp; pp = &(*pp)->next) {
        segment_t *s = *pp;
        if (seg_first(s) != c)
            continue;
        if (s->from_sbrk)
            return 0;
        *pp = s->next;
        a->heap_bytes -= s->size;
        os_sub(s->size);
        munmap(s->base, s->size);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Core allocate / free (arena lock held)                                   */
/* ------------------------------------------------------------------------ */

static chunk_t *alloc_from_top(arena_t *a, size_t nb)
{
    chunk_t *t = a->top;
    if (!t || csize(t) < nb + MIN_CHUNK) {
        if (grow_arena(a, nb) != 0)
            return NULL;
        t = a->top;
    }
    size_t rs = csize(t) - nb;
    chunk_t *rest = c_at(t, nb);
    set_head(t, nb, F_INUSE | F_PREV_INUSE, a->id);
    set_head(rest, rs, F_PREV_INUSE, a->id);
    c_next(rest)->prev_size = rs;
    a->top = rest;
    return t;
}

static chunk_t *arena_alloc(arena_t *a, size_t nb)
{
    unsigned i = bin_index(nb);
    chunk_t *c = NULL;

    if (nb < SMALL_LIMIT) {
        c = a->bins[i];             /* exact-size bin */
        if (c) {
            bin_unlink(a, c);
            c->head |= F_INUSE;
            c_next(c)->head |= F_PREV_INUSE;
            a->inuse_bytes += nb;
            return c;
        }
        i++;
    } else {
        /* Bounded best-fit within the request's own large bin. */
        chunk_t *best = NULL;
        size_t bsz = SIZE_MAX;
        int scanned = 0;
        for (chunk_t *p = a->bins[i]; p && scanned < LARGE_SCAN_MAX; p = p->fd, scanned++) {
            size_t ps = csize(p);
            if (ps >= nb && ps < bsz) {
                best = p;
                bsz = ps;
                if (ps == nb)
                    break;
            }
        }
        c = best;
        i++;
    }

    if (!c) {
        /* Any chunk in a higher bin is big enough. */
        int j = next_bin(a, i);
        if (j < 0) {
            c = alloc_from_top(a, nb);
            if (c)
                a->inuse_bytes += nb;
            return c;
        }
        c = a->bins[j];
    }

    bin_unlink(a, c);
    size_t cs = csize(c);
    if (cs - nb >= MIN_CHUNK) {
        /* Split: c's successor is in use (free chunks are never adjacent to
         * each other or to top), so the remainder can go straight to a bin. */
        set_head(c, nb, F_INUSE | F_PREV_INUSE, a->id);
        chunk_t *r = c_at(c, nb);
        mark_free(a, r, cs - nb);
        bin_insert(a, r);
    } else {
        c->head |= F_INUSE;
        c_next(c)->head |= F_PREV_INUSE;
    }
    a->inuse_bytes += csize(c);
    return c;
}

/* Free an in-use chunk, coalescing with free neighbours and top. */
static void arena_free(arena_t *a, chunk_t *c)
{
    size_t sz = csize(c);
    a->inuse_bytes -= sz;
    chunk_t *n = c_at(c, sz);

    if (!c_prev_inuse(c)) {
        chunk_t *p = c_prev(c);
        if (csize(p) != c->prev_size || c_inuse(p))
            corruption("corrupted prev_size", c2mem(c));
        bin_unlink(a, p);
        sz += c->prev_size;
        c = p;
    }

    if (n == a->top) {
        sz += csize(n);
        set_head(c, sz, F_PREV_INUSE, a->id);
        c_at(c, sz)->prev_size = sz;
        a->top = c;
        maybe_trim(a);
        return;
    }
    if (!c_inuse(n)) {
        bin_unlink(a, n);
        sz += csize(n);
    }
    mark_free(a, c, sz);
    if (csize(c_at(c, sz)) == 0 && release_segment(a, c))
        return;
    bin_insert(a, c);
}

/* Shrink in-use chunk c to nb bytes, freeing the tail if it is big enough. */
static void arena_shrink(arena_t *a, chunk_t *c, size_t nb)
{
    size_t cs = csize(c);
    if (cs - nb < MIN_CHUNK)
        return;
    c->head = (c->head & ~SIZE_MASK) | nb;
    chunk_t *r = c_at(c, nb);
    set_head(r, cs - nb, F_INUSE | F_PREV_INUSE, a->id);
    arena_free(a, r);
}

/* ------------------------------------------------------------------------ */
/* Direct mmap for large requests                                           */
/* ------------------------------------------------------------------------ */

static void *mmap_alloc(size_t n, size_t align)
{
    size_t extra = align > MA_ALIGN ? align : 0;
    if (n > SIZE_MAX - HDR_SIZE - extra - page_size)
        return NULL;
    size_t len = align_up(n + HDR_SIZE + extra, page_size);
    char *base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return NULL;
    VG_NOACCESS(base, len);
    char *mem = base + HDR_SIZE;
    if (align > MA_ALIGN)
        mem = (char *)align_up((uintptr_t)mem, align);
    chunk_t *c = mem2c(mem);
    size_t off = (size_t)((char *)c - base);
    c->prev_size = off;                       /* offset back to mapping start */
    set_head(c, len - off, F_INUSE | F_MMAPPED, 0);
    atomic_fetch_add_explicit(&g_mmap_bytes, len, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_mmap_count, 1, memory_order_relaxed);
    os_add(len);
    return mem;
}

static void mmap_free(chunk_t *c)
{
    size_t len = csize(c) + c->prev_size;
    atomic_fetch_sub_explicit(&g_mmap_bytes, len, memory_order_relaxed);
    atomic_fetch_sub_explicit(&g_mmap_count, 1, memory_order_relaxed);
    os_sub(len);
    munmap((char *)c - c->prev_size, len);
}

/* ------------------------------------------------------------------------ */
/* Internal entry points (no valgrind annotations)                          */
/* ------------------------------------------------------------------------ */

static inline arena_t *owner_of(chunk_t *c)
{
    unsigned id = c_arena(c);
    if (id >= narenas)
        corruption("free(): invalid pointer", c2mem(c));
    return &arenas[id];
}

/* The calling thread's cache, or NULL if caching is off for this thread. */
static inline tcache_t *tc_get(void)
{
    tcache_t *tc = &tcache;
    if (__builtin_expect(tc->state == TC_ON, 1))
        return tc;
    if (tc->state == TC_OFF)
        return NULL;
    pthread_once(&init_once, ma_init);
    if (!tcache_enabled) {
        tc->state = TC_OFF;
        return NULL;
    }
    tc->state = TC_ON;        /* before setspecific, which may allocate */
    pthread_setspecific(tcache_key, tc);
    return tc;
}

static inline void tc_push(tcache_t *tc, unsigned i, chunk_t *c)
{
    c->fd = tc->head[i];
    c->bk = (chunk_t *)tcache_cookie;
    tc->head[i] = c;
    tc->count[i]++;
}

/* Move up to half a cache's worth of same-size chunks from the arena's exact
 * bin into the cache while we hold the lock anyway. */
static void tc_refill(arena_t *a, tcache_t *tc, size_t nb)
{
    unsigned i = bin_index(nb);
    chunk_t *c;
    while (tc->count[i] < TCACHE_COUNT / 2 && (c = a->bins[i]) != NULL) {
        bin_unlink(a, c);
        c->head |= F_INUSE;
        c_next(c)->head |= F_PREV_INUSE;
        a->inuse_bytes += nb;
        tc_push(tc, i, c);
    }
}

/* Return every cached chunk to its owning arena. */
static void tc_drain(tcache_t *tc)
{
    for (unsigned i = 0; i < NSMALL; i++) {
        chunk_t *c;
        while ((c = tc->head[i]) != NULL) {
            tc->head[i] = c->fd;
            tc->count[i]--;
            c->bk = NULL;
            arena_t *a = owner_of(c);
            pthread_mutex_lock(&a->lock);
            arena_free(a, c);
            pthread_mutex_unlock(&a->lock);
        }
    }
}

/* pthread key destructor: a thread's cache dies with it. Caching stays off
 * for any allocations later destructors make on this thread. */
static void tcache_thread_exit(void *arg)
{
    tcache_t *tc = arg;
    tc->state = TC_OFF;
    VG_ENTER();
    tc_drain(tc);
    VG_LEAVE();
}

static void *do_malloc(size_t n)
{
    size_t nb;
    if (req2size(n, &nb) != 0) {
        errno = ENOMEM;
        return NULL;
    }
    if (nb >= MMAP_THRESHOLD) {
        void *p = mmap_alloc(n, 0);
        if (!p)
            errno = ENOMEM;
        return p;
    }
    tcache_t *tc = nb < SMALL_LIMIT ? tc_get() : NULL;
    if (tc) {
        unsigned i = bin_index(nb);
        chunk_t *c = tc->head[i];
        if (c) {
            tc->head[i] = c->fd;
            tc->count[i]--;
            c->bk = NULL;
            return c2mem(c);
        }
    }
    arena_t *a = lock_my_arena();
    chunk_t *c = arena_alloc(a, nb);
    if (c && tc)
        tc_refill(a, tc, nb);
    pthread_mutex_unlock(&a->lock);
    if (!c) {
        errno = ENOMEM;
        return NULL;
    }
    return c2mem(c);
}

static void do_free(void *p)
{
    chunk_t *c = mem2c(p);
    if (!c_inuse(c) || ((uintptr_t)p & (MA_ALIGN - 1)))
        corruption("free(): invalid pointer or double free", p);
    if (c_mmapped(c)) {
        mmap_free(c);
        return;
    }
    size_t sz = csize(c);
    tcache_t *tc = sz < SMALL_LIMIT ? tc_get() : NULL;
    if (tc) {
        unsigned i = bin_index(sz);
        if ((uintptr_t)c->bk == tcache_cookie) {
            /* Probably already cached: confirm before calling it a bug. */
            for (chunk_t *e = tc->head[i]; e; e = e->fd)
                if (e == c)
                    corruption("free(): double free detected in tcache", p);
        }
        if (tc->count[i] < TCACHE_COUNT) {
            tc_push(tc, i, c);
            return;
        }
    }
    arena_t *a = owner_of(c);
    pthread_mutex_lock(&a->lock);
    if (!c_inuse(c))
        corruption("double free", p);
    arena_free(a, c);
    pthread_mutex_unlock(&a->lock);
}

static size_t usable_size(chunk_t *c)
{
    return c_mmapped(c) ? csize(c) - HDR_SIZE : csize(c) - sizeof(size_t);
}

#ifndef MA_VALGRIND
static void *do_realloc(void *p, size_t n)
{
    chunk_t *c = mem2c(p);
    if (!c_inuse(c))
        corruption("realloc(): invalid pointer", p);
    size_t nb;
    if (req2size(n, &nb) != 0) {
        errno = ENOMEM;
        return NULL;
    }

    if (c_mmapped(c)) {
        if (nb >= MMAP_THRESHOLD && c->prev_size == 0) {
            size_t oldlen = csize(c);
            size_t newlen = align_up(n + HDR_SIZE, page_size);
            if (newlen == oldlen)
                return p;
            char *q = mremap(c, oldlen, newlen, MREMAP_MAYMOVE);
            if (q == MAP_FAILED) {
                errno = ENOMEM;
                return NULL;
            }
            chunk_t *nc = (chunk_t *)q;
            set_head(nc, newlen, F_INUSE | F_MMAPPED, 0);
            if (newlen > oldlen) {
                atomic_fetch_add_explicit(&g_mmap_bytes, newlen - oldlen, memory_order_relaxed);
                os_add(newlen - oldlen);
            } else {
                atomic_fetch_sub_explicit(&g_mmap_bytes, oldlen - newlen, memory_order_relaxed);
                os_sub(oldlen - newlen);
            }
            return c2mem(nc);
        }
    } else {
        arena_t *a = owner_of(c);
        pthread_mutex_lock(&a->lock);
        size_t cs = csize(c);
        chunk_t *n2 = c_at(c, cs);
        if (cs >= nb) {
            arena_shrink(a, c, nb);
            pthread_mutex_unlock(&a->lock);
            return p;
        }
        if (n2 == a->top && cs + csize(n2) >= nb + MIN_CHUNK) {
            /* Grow in place into the wilderness. */
            size_t rs = cs + csize(n2) - nb;
            c->head = (c->head & ~SIZE_MASK) | nb;
            chunk_t *rest = c_at(c, nb);
            set_head(rest, rs, F_PREV_INUSE, a->id);
            c_next(rest)->prev_size = rs;
            a->top = rest;
            a->inuse_bytes += nb - cs;
            pthread_mutex_unlock(&a->lock);
            return p;
        }
        if (n2 != a->top && !c_inuse(n2) && cs + csize(n2) >= nb) {
            /* Absorb the free successor, then trim back to size. */
            size_t merged = cs + csize(n2);
            bin_unlink(a, n2);
            c->head = (c->head & ~SIZE_MASK) | merged;
            c_at(c, merged)->head |= F_PREV_INUSE;
            a->inuse_bytes += merged - cs;
            arena_shrink(a, c, nb);
            pthread_mutex_unlock(&a->lock);
            return p;
        }
        pthread_mutex_unlock(&a->lock);
    }

    void *q = do_malloc(n);
    if (!q)
        return NULL;
    size_t old = usable_size(c);
    memcpy(q, p, old < n ? old : n);
    do_free(p);
    return q;
}
#endif

static void *do_memalign(size_t align, size_t n)
{
    if (align <= MA_ALIGN)
        return do_malloc(n);
    size_t nb;
    if (req2size(n, &nb) != 0 || align > MAX_REQUEST || nb + align + MIN_CHUNK >= MAX_REQUEST) {
        errno = ENOMEM;
        return NULL;
    }
    if (nb + align >= MMAP_THRESHOLD) {
        void *p = mmap_alloc(n, align);
        if (!p)
            errno = ENOMEM;
        return p;
    }

    /* Over-allocate, then carve an aligned chunk out of the middle and give
     * the leading and trailing slack back to the free lists. */
    arena_t *a = lock_my_arena();
    chunk_t *c = arena_alloc(a, nb + align + MIN_CHUNK);
    if (!c) {
        pthread_mutex_unlock(&a->lock);
        errno = ENOMEM;
        return NULL;
    }
    uintptr_t mem = (uintptr_t)c2mem(c);
    if (mem & (align - 1)) {
        uintptr_t am = align_up(mem, align);
        if (am - mem < MIN_CHUNK)
            am = align_up(mem + MIN_CHUNK, align);
        chunk_t *ac = mem2c((void *)am);
        size_t lead = (size_t)((char *)ac - (char *)c);
        size_t rest = csize(c) - lead;
        size_t pflag = c->head & F_PREV_INUSE;
        set_head(c, lead, F_INUSE | pflag, a->id);
        set_head(ac, rest, F_INUSE | F_PREV_INUSE, a->id);
        arena_free(a, c);   /* lead becomes free and clears ac's PREV_INUSE */
        c = ac;
    }
    arena_shrink(a, c, nb);
    pthread_mutex_unlock(&a->lock);
    return c2mem(c);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

void *ma_malloc(size_t n)
{
    VG_ENTER();
    void *p = do_malloc(n);
    VG_LEAVE();
    if (p)
        VG_ALLOC(p, n, 0);
    return p;
}

void ma_free(void *p)
{
    if (!p)
        return;
    VG_FREE(p);
    VG_ENTER();
    do_free(p);
    VG_LEAVE();
}

void *ma_calloc(size_t count, size_t n)
{
    size_t total;
    if (__builtin_mul_overflow(count, n, &total)) {
        errno = ENOMEM;
        return NULL;
    }
    VG_ENTER();
    void *p = do_malloc(total);
    if (p && !c_mmapped(mem2c(p)))   /* fresh mmap pages are already zero */
        memset(p, 0, total);
    VG_LEAVE();
    if (p)
        VG_ALLOC(p, total, 1);
    return p;
}

void *ma_realloc(void *p, size_t n)
{
    if (!p)
        return ma_malloc(n);
    if (n == 0) {
        ma_free(p);
        return NULL;
    }
#ifdef MA_VALGRIND
    /* memcheck cannot follow in-place resizes without knowing the old
     * request size, so always move: allocate, copy, free. */
    void *q = ma_malloc(n);
    if (q) {
        size_t old = ma_usable_size(p);
        VG_ENTER();
        memcpy(q, p, old < n ? old : n);
        VG_LEAVE();
        ma_free(p);
    }
    return q;
#else
    return do_realloc(p, n);
#endif
}

void *ma_memalign(size_t align, size_t n)
{
    if (align == 0 || (align & (align - 1))) {
        errno = EINVAL;
        return NULL;
    }
    VG_ENTER();
    void *p = do_memalign(align, n);
    VG_LEAVE();
    if (p)
        VG_ALLOC(p, n, 0);
    return p;
}

int ma_posix_memalign(void **out, size_t align, size_t n)
{
    if (align < sizeof(void *) || (align & (align - 1)))
        return EINVAL;
    int saved = errno;
    void *p = ma_memalign(align, n);
    errno = saved;
    if (!p)
        return ENOMEM;
    *out = p;
    return 0;
}

void ma_tcache_flush(void)
{
    if (tcache.state != TC_ON)
        return;
    VG_ENTER();
    tc_drain(&tcache);
    VG_LEAVE();
}

size_t ma_usable_size(void *p)
{
    if (!p)
        return 0;
    VG_ENTER();
    size_t n = usable_size(mem2c(p));
    VG_LEAVE();
    return n;
}

/* ------------------------------------------------------------------------ */
/* Introspection                                                            */
/* ------------------------------------------------------------------------ */

static size_t arena_free_bytes(const arena_t *a)
{
    size_t total = a->top ? csize(a->top) : 0;
    for (unsigned i = 0; i < NBINS; i++)
        for (chunk_t *c = a->bins[i]; c; c = c->fd)
            total += csize(c);
    return total;
}

void ma_get_stats(ma_stats_t *st)
{
    pthread_once(&init_once, ma_init);
    memset(st, 0, sizeof *st);
    VG_ENTER();
    for (unsigned i = 0; i < narenas; i++) {
        arena_t *a = &arenas[i];
        pthread_mutex_lock(&a->lock);
        st->heap_bytes += a->heap_bytes;
        st->inuse_bytes += a->inuse_bytes;
        st->free_bytes += arena_free_bytes(a);
        pthread_mutex_unlock(&a->lock);
    }
    VG_LEAVE();
    st->os_bytes = atomic_load(&g_os_bytes);
    st->peak_os_bytes = atomic_load(&g_peak_os_bytes);
    st->mmap_bytes = atomic_load(&g_mmap_bytes);
    st->mmap_count = atomic_load(&g_mmap_count);
    st->narenas = narenas;
}

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            ma_log(2, "ma_check: arena %u: ", a->id);     \
            ma_log(2, __VA_ARGS__);                       \
            ma_log(2, "\n");                              \
            return -1;                                    \
        }                                                 \
    } while (0)

static int in_arena(const arena_t *a, const chunk_t *c)
{
    for (const segment_t *s = a->segs; s; s = s->next)
        if ((const char *)c >= (const char *)seg_first(s) && c < seg_fence(s))
            return 1;
    return 0;
}

static int check_arena(arena_t *a)
{
    size_t nfree_walk = 0, inuse = 0, heap = 0;
    int top_seen = 0;

    for (segment_t *s = a->segs; s; s = s->next) {
        heap += s->size;
        chunk_t *fence = seg_fence(s);
        chunk_t *c = seg_first(s);
        int prev_free = 0;
        CHECK(c_prev_inuse(c), "segment %p: first chunk lacks PREV_INUSE", (void *)s);
        while (c < fence) {
            size_t sz = csize(c);
            CHECK(((uintptr_t)c & (MA_ALIGN - 1)) == 0, "misaligned chunk %p", (void *)c);
            CHECK(sz >= MIN_CHUNK && (sz & (MA_ALIGN - 1)) == 0, "chunk %p bad size %zu", (void *)c, sz);
            CHECK(c_arena(c) == a->id, "chunk %p has arena %u", (void *)c, c_arena(c));
            CHECK(!c_mmapped(c), "heap chunk %p flagged mmapped", (void *)c);
            CHECK(c_prev_inuse(c) == !prev_free, "chunk %p PREV_INUSE mismatch", (void *)c);
            CHECK((char *)c + sz <= (char *)fence, "chunk %p overruns segment", (void *)c);
            if (c_inuse(c)) {
                inuse += sz;
                prev_free = 0;
            } else {
                CHECK(!prev_free, "adjacent free chunks at %p", (void *)c);
                CHECK(c_next(c)->prev_size == sz, "chunk %p footer mismatch", (void *)c);
                if (c == a->top) {
                    CHECK(s == a->segs && c_next(c) == fence, "top %p not at end of current segment", (void *)c);
                    top_seen = 1;
                } else {
                    nfree_walk++;
                }
                prev_free = 1;
            }
            c = c_next(c);
        }
        CHECK(c == fence, "segment %p: walk ended at %p, fence at %p", (void *)s, (void *)c, (void *)fence);
        CHECK(csize(fence) == 0 && c_inuse(fence), "segment %p: bad fence", (void *)s);
        CHECK(c_prev_inuse(fence) == !prev_free, "segment %p: fence PREV_INUSE mismatch", (void *)s);
    }
    CHECK(top_seen == (a->top != NULL), "top chunk %p not found in walk", (void *)a->top);
    CHECK(inuse == a->inuse_bytes, "inuse_bytes %zu, walk found %zu", a->inuse_bytes, inuse);
    CHECK(heap == a->heap_bytes, "heap_bytes %zu, walk found %zu", a->heap_bytes, heap);

    size_t nfree_bins = 0;
    for (unsigned i = 0; i < NBINS; i++) {
        int bit = (a->binmap[i >> 6] >> (i & 63)) & 1;
        CHECK(bit == (a->bins[i] != NULL), "binmap bit %u wrong", i);
        chunk_t *prev = NULL;
        for (chunk_t *c = a->bins[i]; c; prev = c, c = c->fd) {
            CHECK(in_arena(a, c), "bin %u entry %p outside arena", i, (void *)c);
            CHECK(!c_inuse(c), "bin %u entry %p is in use", i, (void *)c);
            CHECK(c != a->top, "top chunk is in bin %u", i);
            CHECK(bin_index(csize(c)) == i, "chunk %p size %zu in wrong bin %u", (void *)c, csize(c), i);
            CHECK(c->bk == prev, "bin %u: broken bk link at %p", i, (void *)c);
            nfree_bins++;
        }
    }
    CHECK(nfree_bins == nfree_walk, "%zu free chunks in bins, %zu in heap", nfree_bins, nfree_walk);
    return 0;
}

int ma_check(void)
{
    pthread_once(&init_once, ma_init);
    int rc = 0;
    VG_ENTER();
    for (unsigned i = 0; i < narenas && rc == 0; i++) {
        arena_t *a = &arenas[i];
        pthread_mutex_lock(&a->lock);
        rc = check_arena(a);
        pthread_mutex_unlock(&a->lock);
    }
    VG_LEAVE();
    return rc;
}

void ma_dump(int fd)
{
    pthread_once(&init_once, ma_init);
    VG_ENTER();
    for (unsigned i = 0; i < narenas; i++) {
        arena_t *a = &arenas[i];
        pthread_mutex_lock(&a->lock);
        if (a->segs) {
            ma_log(fd, "arena %u: heap=%zu inuse=%zu free=%zu top=%p\n", a->id, a->heap_bytes,
                   a->inuse_bytes, arena_free_bytes(a), (void *)a->top);
            for (segment_t *s = a->segs; s; s = s->next) {
                ma_log(fd, "  segment %p-%p (%zu bytes, %s)\n", (void *)s->base,
                       (void *)(s->base + s->size), s->size, s->from_sbrk ? "sbrk" : "mmap");
                int shown = 0;
                for (chunk_t *c = seg_first(s); csize(c) != 0; c = c_next(c)) {
                    if (shown++ == 64) {
                        ma_log(fd, "    ...\n");
                        break;
                    }
                    ma_log(fd, "    %p %8zu %s\n", (void *)c, csize(c),
                           c == a->top ? "top" : c_inuse(c) ? "used" : "free");
                }
            }
        }
        pthread_mutex_unlock(&a->lock);
    }
    VG_LEAVE();
    ma_log(fd, "mmapped: %zu chunks, %zu bytes; os total %zu bytes (peak %zu)\n",
           atomic_load(&g_mmap_count), atomic_load(&g_mmap_bytes), atomic_load(&g_os_bytes),
           atomic_load(&g_peak_os_bytes));
}
