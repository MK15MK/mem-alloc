/*
 * Benchmarks ma against glibc malloc. Each invocation exercises a single
 * allocator so the two never share a process (or a brk heap):
 *
 *     bench <ma|glibc> <workload> [threads]
 *
 * Workloads:
 *   small    single thread, random malloc/free of 16..256 byte blocks
 *   mixed    single thread, 70% <128B, 25% <4KB, 5% <64KB, writes each block
 *   realloc  single thread, buffers grown by realloc in random steps
 *   threads  N threads each running `mixed` on private data
 *   xthread  N/2 producer threads malloc, N/2 consumer threads free
 *   frag     phased alloc/free trace; reports memory footprint vs live bytes
 *
 * Output is one CSV line per metric: allocator,workload,threads,metric,value
 */
#define _GNU_SOURCE
#include "ma.h"

#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const char *name;
    void *(*malloc)(size_t);
    void (*free)(void *);
    void *(*realloc)(void *, size_t);
    size_t (*footprint)(void);
} allocator_t;

static size_t glibc_footprint(void)
{
    struct mallinfo2 mi = mallinfo2();
    return mi.arena + mi.hblkhd;
}

static size_t ma_footprint(void)
{
    ma_stats_t st;
    ma_get_stats(&st);
    return st.os_bytes;
}

static const allocator_t allocators[] = {
    {"glibc", malloc, free, realloc, glibc_footprint},
    {"ma", ma_malloc, ma_free, ma_realloc, ma_footprint},
};

/* Benchmark bookkeeping comes straight from mmap so neither allocator
 * under test is charged for it (or perturbed by it). */
static void *raw_alloc(size_t n)
{
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    return p;
}

static const allocator_t *A;
static const char *workload;
static int nthreads = 1;

static void report(const char *metric, double value)
{
    printf("%s,%s,%d,%s,%.4f\n", A->name, workload, nthreads, metric, value);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static size_t rss_bytes(void)
{
    long pages_total, pages_rss;
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f || fscanf(f, "%ld %ld", &pages_total, &pages_rss) != 2) {
        if (f)
            fclose(f);
        return 0;
    }
    fclose(f);
    return (size_t)pages_rss * (size_t)sysconf(_SC_PAGESIZE);
}

static inline uint64_t next_rand(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

static inline size_t mixed_size(uint64_t *r)
{
    uint64_t v = next_rand(r);
    uint64_t pct = v % 100, x = v >> 8;
    if (pct < 70)
        return x % 128 + 1;
    if (pct < 95)
        return x % 4096 + 1;
    return x % 65536 + 1;
}

/* ---- single-threaded throughput ---------------------------------------- */

static void bench_small(void)
{
    enum { SLOTS = 1024 };
    const long ops = 40000000;
    void *slot[SLOTS] = {0};
    uint64_t r = 42;
    double t0 = now();
    for (long i = 0; i < ops; i++) {
        uint64_t v = next_rand(&r);
        unsigned k = v % SLOTS;
        if (slot[k]) {
            A->free(slot[k]);
            slot[k] = NULL;
        } else {
            slot[k] = A->malloc(16 + (v >> 16) % 241);
        }
    }
    double dt = now() - t0;
    for (int k = 0; k < SLOTS; k++)
        A->free(slot[k]);
    report("mops_per_sec", ops / dt / 1e6);
}

static long run_mixed(long ops, uint64_t seed)
{
    enum { SLOTS = 8192 };
    void **slot = raw_alloc(SLOTS * sizeof *slot);
    uint64_t r = seed;
    for (long i = 0; i < ops; i++) {
        uint64_t v = next_rand(&r);
        unsigned k = v % SLOTS;
        if (slot[k]) {
            A->free(slot[k]);
            slot[k] = NULL;
        } else {
            size_t n = mixed_size(&r);
            char *p = A->malloc(n);
            p[0] = p[n - 1] = (char)v;   /* touch both ends */
            slot[k] = p;
        }
    }
    for (int k = 0; k < SLOTS; k++)
        A->free(slot[k]);
    munmap(slot, SLOTS * sizeof *slot);
    return ops;
}

static void bench_mixed(void)
{
    const long ops = 20000000;
    double t0 = now();
    run_mixed(ops, 7);
    report("mops_per_sec", ops / (now() - t0) / 1e6);
}

static void bench_realloc(void)
{
    enum { BUFS = 2000 };
    const long ops = 10000000;
    char *buf[BUFS] = {0};
    size_t len[BUFS] = {0};
    uint64_t r = 99;
    double t0 = now();
    for (long i = 0; i < ops; i++) {
        uint64_t v = next_rand(&r);
        unsigned k = v % BUFS;
        size_t grow = 1 + (v >> 20) % 64;
        if (len[k] + grow > 32768) {
            A->free(buf[k]);
            buf[k] = NULL;
            len[k] = 0;
            continue;
        }
        buf[k] = A->realloc(buf[k], len[k] + grow);
        buf[k][len[k] + grow - 1] = (char)v;
        len[k] += grow;
    }
    double dt = now() - t0;
    for (int k = 0; k < BUFS; k++)
        A->free(buf[k]);
    report("mops_per_sec", ops / dt / 1e6);
}

/* ---- multithreaded throughput ------------------------------------------ */

static const long THREAD_OPS = 5000000;

static void *threads_worker(void *arg)
{
    run_mixed(THREAD_OPS, 1000 + (uintptr_t)arg);
    return NULL;
}

static void bench_threads(void)
{
    pthread_t th[64];
    double t0 = now();
    for (long i = 0; i < nthreads; i++)
        pthread_create(&th[i], NULL, threads_worker, (void *)i);
    for (int i = 0; i < nthreads; i++)
        pthread_join(th[i], NULL);
    double dt = now() - t0;
    report("mops_per_sec", (double)THREAD_OPS * nthreads / dt / 1e6);
}

/* Producer/consumer pairs connected by single-producer/single-consumer
 * rings: every block is freed by a different thread than allocated it. */
#define RING 4096
typedef struct {
    _Atomic size_t head;
    char pad1[56];
    _Atomic size_t tail;
    char pad2[56];
    void *slot[RING];
} ring_t;

static const long XFER_BLOCKS = 4000000;

static void *producer(void *arg)
{
    ring_t *q = arg;
    uint64_t r = (uintptr_t)arg;
    for (long i = 0; i < XFER_BLOCKS; i++) {
        size_t n = 16 + next_rand(&r) % 497;
        char *p = A->malloc(n);
        p[0] = 1;
        size_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
        while (h - atomic_load_explicit(&q->tail, memory_order_acquire) == RING)
            ;
        q->slot[h % RING] = p;
        atomic_store_explicit(&q->head, h + 1, memory_order_release);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    ring_t *q = arg;
    for (long i = 0; i < XFER_BLOCKS; i++) {
        size_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
        while (atomic_load_explicit(&q->head, memory_order_acquire) == t)
            ;
        A->free(q->slot[t % RING]);
        atomic_store_explicit(&q->tail, t + 1, memory_order_release);
    }
    return NULL;
}

static void bench_xthread(void)
{
    int pairs = nthreads / 2 > 0 ? nthreads / 2 : 1;
    ring_t *rings = raw_alloc(sizeof(ring_t) * (size_t)pairs);
    pthread_t th[64];
    double t0 = now();
    for (int i = 0; i < pairs; i++) {
        pthread_create(&th[2 * i], NULL, producer, &rings[i]);
        pthread_create(&th[2 * i + 1], NULL, consumer, &rings[i]);
    }
    for (int i = 0; i < 2 * pairs; i++)
        pthread_join(th[i], NULL);
    double dt = now() - t0;
    munmap(rings, sizeof(ring_t) * (size_t)pairs);
    /* one malloc + one free per block */
    report("mops_per_sec", 2.0 * XFER_BLOCKS * pairs / dt / 1e6);
}

/* ---- fragmentation ----------------------------------------------------- */

/* Alternates between filling the heap to 64 MiB of live data and freeing
 * random blocks down to 16 MiB. The size distribution shifts every round,
 * so holes left by one round's small blocks must serve the next round's
 * larger ones (or be wasted). Footprint is measured both from the
 * allocator's own accounting and from process RSS. */
static void bench_frag(void)
{
    const size_t target = 64u << 20;
    const int rounds = 20;
    enum { MAXLIVE = 1 << 20 };
    void **ptr = raw_alloc(sizeof(void *) * MAXLIVE);
    size_t *sz = raw_alloc(sizeof(size_t) * MAXLIVE);
    size_t nlive = 0, live = 0, peak_live = 0, peak_fp = 0, peak_rss = 0;
    double ratio_hi = 0, ratio_lo = 0, rss_ratio_hi = 0, rss_ratio_lo = 0;
    uint64_t r = 2024;
    size_t rss0 = rss_bytes();
    double t0 = now();

    for (int round = 0; round < rounds; round++) {
        size_t lo = (size_t)16 << (round % 6);       /* 16 B .. 512 B */
        size_t hi = lo * 32;                          /* 512 B .. 16 KiB */
        while (live < target && nlive < MAXLIVE) {
            uint64_t v = next_rand(&r);
            /* log-uniform in [lo, hi) */
            double f = (double)(v >> 11) / (double)(1ull << 53);
            size_t n = (size_t)((double)lo * pow((double)hi / (double)lo, f));
            char *p = A->malloc(n);
            memset(p, (int)round, n);
            ptr[nlive] = p;
            sz[nlive] = n;
            nlive++;
            live += n;
        }
        size_t fp = A->footprint(), rss = rss_bytes() - rss0;
        if (live > peak_live)
            peak_live = live;
        if (fp > peak_fp)
            peak_fp = fp;
        if (rss > peak_rss)
            peak_rss = rss;
        ratio_hi += (double)fp / live;
        rss_ratio_hi += (double)rss / live;

        while (live > target / 4) {
            size_t k = next_rand(&r) % nlive;
            A->free(ptr[k]);
            live -= sz[k];
            nlive--;
            ptr[k] = ptr[nlive];
            sz[k] = sz[nlive];
        }
        ratio_lo += (double)A->footprint() / live;
        rss_ratio_lo += (double)(rss_bytes() - rss0) / live;
    }
    double dt = now() - t0;
    for (size_t k = 0; k < nlive; k++)
        A->free(ptr[k]);
    size_t end_fp = A->footprint();
    munmap(ptr, sizeof(void *) * MAXLIVE);
    munmap(sz, sizeof(size_t) * MAXLIVE);

    report("peak_live_mib", peak_live / 1048576.0);
    report("peak_footprint_mib", peak_fp / 1048576.0);
    report("peak_rss_mib", peak_rss / 1048576.0);
    report("footprint_over_live_at_peaks", ratio_hi / rounds);
    report("footprint_over_live_at_troughs", ratio_lo / rounds);
    report("rss_over_live_at_peaks", rss_ratio_hi / rounds);
    report("rss_over_live_at_troughs", rss_ratio_lo / rounds);
    report("footprint_after_free_all_mib", end_fp / 1048576.0);
    report("seconds", dt);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <ma|glibc> <small|mixed|realloc|threads|xthread|frag> [threads]\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < sizeof allocators / sizeof *allocators; i++)
        if (strcmp(argv[1], allocators[i].name) == 0)
            A = &allocators[i];
    workload = argv[2];
    if (argc > 3)
        nthreads = atoi(argv[3]);
    if (!A || nthreads < 1 || nthreads > 64) {
        fprintf(stderr, "bad allocator or thread count\n");
        return 2;
    }

    if (!strcmp(workload, "small"))
        bench_small();
    else if (!strcmp(workload, "mixed"))
        bench_mixed();
    else if (!strcmp(workload, "realloc"))
        bench_realloc();
    else if (!strcmp(workload, "threads"))
        bench_threads();
    else if (!strcmp(workload, "xthread"))
        bench_xthread();
    else if (!strcmp(workload, "frag"))
        bench_frag();
    else {
        fprintf(stderr, "unknown workload %s\n", workload);
        return 2;
    }
    return 0;
}
