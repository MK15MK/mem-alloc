/* Single-threaded functional tests for ma. Run with --quick under valgrind. */
#define _GNU_SOURCE
#include "ma.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static int quick;

#define EXPECT(cond)                                                       \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

#define HEAP_OK() EXPECT(ma_check() == 0)

static uint64_t rng = 88172645463325252ull;
static uint64_t xorshift(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void fill(void *p, size_t n, unsigned seed)
{
    unsigned char *b = p;
    for (size_t i = 0; i < n; i++)
        b[i] = (unsigned char)(seed + i * 7);
}

static int verify(const void *p, size_t n, unsigned seed)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++)
        if (b[i] != (unsigned char)(seed + i * 7))
            return 0;
    return 1;
}

static void test_basic(void)
{
    size_t sizes[] = {1, 7, 8, 15, 16, 17, 24, 31, 100, 1000, 1023, 1024, 4096, 65536, 131071, 200000};
    void *ptrs[sizeof sizes / sizeof *sizes];
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        ptrs[i] = ma_malloc(sizes[i]);
        EXPECT(ptrs[i] != NULL);
        EXPECT(((uintptr_t)ptrs[i] & 15) == 0);
        EXPECT(ma_usable_size(ptrs[i]) >= sizes[i]);
        fill(ptrs[i], sizes[i], (unsigned)i);
    }
    HEAP_OK();
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        EXPECT(verify(ptrs[i], sizes[i], (unsigned)i));
        ma_free(ptrs[i]);
    }
    HEAP_OK();
}

static void test_zero_and_null(void)
{
    void *a = ma_malloc(0), *b = ma_malloc(0);
    EXPECT(a && b && a != b);
    ma_free(a);
    ma_free(b);
    ma_free(NULL);
    EXPECT(ma_malloc((size_t)-1) == NULL && errno == ENOMEM);
    HEAP_OK();
}

static void test_calloc(void)
{
    void *d = ma_malloc(512);
    memset(d, 0xAB, 512);
    ma_free(d);
    unsigned char *z = ma_calloc(64, 8);      /* likely reuses the dirty chunk */
    EXPECT(z != NULL);
    for (int i = 0; i < 512; i++)
        EXPECT(z[i] == 0);
    ma_free(z);

    unsigned char *big = ma_calloc(1, 1 << 20);
    EXPECT(big && big[0] == 0 && big[(1 << 20) - 1] == 0);
    ma_free(big);

    errno = 0;
    EXPECT(ma_calloc(SIZE_MAX / 2, 4) == NULL && errno == ENOMEM);
    HEAP_OK();
}

/* Freeing three neighbours in any order must leave one merged chunk. */
static void test_coalesce(void)
{
    char *a = ma_malloc(100), *b = ma_malloc(100), *c = ma_malloc(100);
    char *guard = ma_malloc(100);
    size_t chunk = (size_t)(b - a);
    EXPECT(chunk == 112 && (size_t)(c - b) == chunk);
    ma_free(a);
    ma_free(c);
    ma_free(b);
    HEAP_OK();
    char *m = ma_malloc(3 * chunk - 8);       /* exactly the merged chunk */
    EXPECT(m == a);
    ma_free(m);
    ma_free(guard);
    HEAP_OK();
}

/* A big free chunk must be split, handing back its front. */
static void test_split(void)
{
    char *big = ma_malloc(8000);
    char *guard = ma_malloc(16);
    ma_free(big);
    char *s1 = ma_malloc(1000);
    char *s2 = ma_malloc(1000);
    EXPECT(s1 == big);
    EXPECT(s2 == s1 + 1008);                   /* carved from the remainder */
    HEAP_OK();
    ma_free(s1);
    ma_free(s2);
    ma_free(guard);
    HEAP_OK();
}

static void test_realloc(void)
{
    char *p = ma_realloc(NULL, 50);
    fill(p, 50, 3);
    p = ma_realloc(p, 5000);
    EXPECT(verify(p, 50, 3));
    fill(p, 5000, 4);
    char *q = ma_realloc(p, 100);              /* shrink stays in place */
#ifndef MA_VALGRIND
    EXPECT(q == p);
#endif
    EXPECT(verify(q, 100, 4));
    HEAP_OK();

    /* Growing the most recent chunk extends into top in place. */
    char *last = ma_malloc(256);
    fill(last, 256, 5);
    char *grown = ma_realloc(last, 20000);
#ifndef MA_VALGRIND
    EXPECT(grown == last);
#endif
    EXPECT(verify(grown, 256, 5));

    /* Growing into a freed successor also stays in place. */
    char *x = ma_malloc(200), *y = ma_malloc(200), *g = ma_malloc(16);
    fill(x, 200, 6);
    ma_free(y);
    char *x2 = ma_realloc(x, 350);
#ifndef MA_VALGRIND
    EXPECT(x2 == x);
#endif
    EXPECT(verify(x2, 200, 6));
    HEAP_OK();

    /* Large (mmapped) realloc keeps contents through mremap. */
    char *m = ma_malloc(300000);
    fill(m, 300000, 7);
    m = ma_realloc(m, 3000000);
    EXPECT(verify(m, 300000, 7));
    m = ma_realloc(m, 1000);                   /* back into the heap */
    EXPECT(verify(m, 1000, 7));

    EXPECT(ma_realloc(q, 0) == NULL);
    ma_free(grown);
    ma_free(x2);
    ma_free(g);
    ma_free(m);
    HEAP_OK();
}

static void test_memalign(void)
{
    for (size_t align = 32; align <= 65536; align <<= 1) {
        size_t sizes[] = {1, 100, 4000, 50000};
        for (int i = 0; i < 4; i++) {
            void *p = ma_memalign(align, sizes[i]);
            EXPECT(p && ((uintptr_t)p & (align - 1)) == 0);
            fill(p, sizes[i], (unsigned)align);
            HEAP_OK();
            EXPECT(verify(p, sizes[i], (unsigned)align));
            ma_free(p);
        }
    }
    void *p;
    EXPECT(ma_posix_memalign(&p, 24, 10) == EINVAL);
    EXPECT(ma_posix_memalign(&p, 4096, 10) == 0 && ((uintptr_t)p & 4095) == 0);
    ma_free(p);
    void *big = ma_memalign(1 << 16, 1 << 20);          /* mmapped with offset */
    EXPECT(big && ((uintptr_t)big & 0xFFFF) == 0);
    fill(big, 1 << 20, 9);
    big = ma_realloc(big, 2 << 20);
    EXPECT(verify(big, 1 << 20, 9));
    ma_free(big);
    HEAP_OK();
}

static void test_large_mmap(void)
{
    ma_stats_t before, during, after;
    ma_get_stats(&before);
    void *p = ma_malloc(4 << 20);
    memset(p, 1, 4 << 20);
    ma_get_stats(&during);
    EXPECT(during.mmap_count == before.mmap_count + 1);
    EXPECT(during.mmap_bytes >= before.mmap_bytes + (4 << 20));
    ma_free(p);
    ma_get_stats(&after);
    EXPECT(after.mmap_count == before.mmap_count && after.mmap_bytes == before.mmap_bytes);
}

/* Freeing everything at the end of the heap must release memory. */
static void test_trim(void)
{
    enum { N = 4000 };
    static void *p[N];
    ma_stats_t s0, s1, s2;
    ma_get_stats(&s0);
    for (int i = 0; i < N; i++)
        p[i] = ma_malloc(1000);
    ma_get_stats(&s1);
    EXPECT(s1.heap_bytes >= (size_t)N * 1000);
    for (int i = N; i-- > 0;)
        ma_free(p[i]);
    ma_get_stats(&s2);
    EXPECT(s2.heap_bytes < s1.heap_bytes - N * 900);
    HEAP_OK();
}

/* Random malloc/free/realloc with content verification and periodic
 * full-heap consistency checks. */
static void test_stress(void)
{
    enum { SLOTS = 2048 };
    static void *p[SLOTS];
    static size_t sz[SLOTS];
    int ops = quick ? 20000 : 400000;
    for (int i = 0; i < ops; i++) {
        int k = (int)(xorshift() % SLOTS);
        if (p[k]) {
            EXPECT(verify(p[k], sz[k], (unsigned)k));
            if (xorshift() % 3 == 0) {
                size_t n = (size_t)(xorshift() % 3000) + 1;
                p[k] = ma_realloc(p[k], n);
                EXPECT(verify(p[k], n < sz[k] ? n : sz[k], (unsigned)k));
                sz[k] = n;
                fill(p[k], n, (unsigned)k);
            } else {
                ma_free(p[k]);
                p[k] = NULL;
            }
        } else {
            uint64_t r = xorshift() % 100;
            size_t n = r < 70 ? xorshift() % 128 + 1
                     : r < 95 ? xorshift() % 4096 + 1
                     : r < 99 ? xorshift() % 65536 + 1
                     : xorshift() % 400000 + 1;
            p[k] = r == 42 ? ma_memalign(256, n) : ma_malloc(n);
            sz[k] = n;
            fill(p[k], n, (unsigned)k);
        }
        if (i % (quick ? 2000 : 10000) == 0 && ma_check() != 0) {
            failures++;
            return;
        }
    }
    for (int k = 0; k < SLOTS; k++) {
        if (p[k])
            EXPECT(verify(p[k], sz[k], (unsigned)k));
        ma_free(p[k]);
        p[k] = NULL;
    }
    HEAP_OK();
    ma_stats_t st;
    ma_get_stats(&st);
    EXPECT(st.inuse_bytes == 0 || st.inuse_bytes < 64 * 1024);  /* earlier tests' leftovers */
}

static void test_fork(void)
{
    void *p = ma_malloc(1234);
    pid_t pid = fork();
    if (pid == 0) {
        void *q = ma_malloc(4321);
        ma_free(p);
        ma_free(q);
        _exit(ma_check() == 0 ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ma_free(p);
}

int main(int argc, char **argv)
{
    quick = argc > 1 && strcmp(argv[1], "--quick") == 0;
    test_basic();
    test_zero_and_null();
    test_calloc();
    test_coalesce();
    test_split();
    test_realloc();
    test_memalign();
    test_large_mmap();
    test_trim();
    test_stress();
    test_fork();
    if (failures) {
        printf("test_ma: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_ma: all tests passed\n");
    return 0;
}
