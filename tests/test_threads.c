/* Multithreaded stress test: private churn plus cross-thread frees.
 *
 * Each block records its own size and a seed in its first bytes, and the
 * rest is filled with a pattern derived from them. Threads swap blocks
 * through a shared exchange table, so most frees hit a different arena than
 * the freeing thread's own. */
#define _GNU_SOURCE
#include "ma.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTHREADS 8
#define OPS      300000
#define SLOTS    512
#define XCHG     1024

typedef struct {
    size_t size;
    uint32_t seed;
} tag_t;

static _Atomic(void *) exchange[XCHG];
static atomic_int errors;

static void *make_block(size_t n, uint32_t seed)
{
    if (n < sizeof(tag_t))
        n = sizeof(tag_t);
    unsigned char *p = ma_malloc(n);
    if (!p)
        return NULL;
    tag_t t = {n, seed};
    memcpy(p, &t, sizeof t);
    for (size_t i = sizeof t; i < n; i++)
        p[i] = (unsigned char)(seed ^ i);
    return p;
}

static void check_and_free(unsigned char *p)
{
    tag_t t;
    memcpy(&t, p, sizeof t);
    for (size_t i = sizeof t; i < t.size; i++) {
        if (p[i] != (unsigned char)(t.seed ^ i)) {
            atomic_fetch_add(&errors, 1);
            break;
        }
    }
    ma_free(p);
}

static void *worker(void *arg)
{
    uint64_t r = 0x9E3779B97F4A7C15ull * ((uintptr_t)arg + 1);
    void *slots[SLOTS] = {0};
    for (int i = 0; i < OPS; i++) {
        r ^= r << 13;
        r ^= r >> 7;
        r ^= r << 17;
        int k = (int)(r % SLOTS);
        size_t n = (r >> 20) % 100 < 90 ? (r >> 32) % 256 + 1 : (r >> 32) % 20000 + 1;
        if ((r >> 40) % 4 == 0) {
            /* hand a block to another thread, take whatever was there */
            void *mine = make_block(n, (uint32_t)r);
            void *theirs = atomic_exchange(&exchange[(r >> 8) % XCHG], mine);
            if (theirs)
                check_and_free(theirs);
        } else if (slots[k]) {
            check_and_free(slots[k]);
            slots[k] = NULL;
        } else {
            slots[k] = make_block(n, (uint32_t)r);
        }
    }
    for (int k = 0; k < SLOTS; k++)
        if (slots[k])
            check_and_free(slots[k]);
    return NULL;
}

int main(void)
{
    pthread_t th[NTHREADS];
    for (long i = 0; i < NTHREADS; i++)
        pthread_create(&th[i], NULL, worker, (void *)i);
    for (int i = 0; i < NTHREADS; i++)
        pthread_join(th[i], NULL);
    for (int i = 0; i < XCHG; i++) {
        void *p = atomic_exchange(&exchange[i], NULL);
        if (p)
            check_and_free(p);
    }
    ma_tcache_flush();   /* blocks freed here sit in main's cache */

    ma_stats_t st;
    ma_get_stats(&st);
    int ok = atomic_load(&errors) == 0 && ma_check() == 0 && st.inuse_bytes == 0;
    printf("test_threads: %d threads x %d ops, %u arenas, corrupted=%d inuse_after=%zu -> %s\n",
           NTHREADS, OPS, st.narenas, atomic_load(&errors), st.inuse_bytes, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
