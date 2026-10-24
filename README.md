# ma: a custom memory allocator in C

`ma` is a general-purpose `malloc`/`free`/`realloc`/`calloc`/`memalign` for
Linux. It uses **segregated free lists**, **block splitting** and
**boundary-tag coalescing** on top of `sbrk`/`mmap`, and **per-arena mutexes**
for thread safety. You can link it directly (`ma_malloc`, ...) or load it in
place of glibc's allocator with `LD_PRELOAD`. Python, gcc, bash, `ls` and
`sort` all run correctly on it.

```
include/ma.h          public API, stats, ma_check / ma_dump
src/ma.c              the allocator (~1100 lines)
src/ma_preload.c      malloc/free/... symbol overrides for LD_PRELOAD
tests/                functional, multithreaded, fork, Valgrind and GDB demos
bench/bench.c         throughput + fragmentation benchmarks vs glibc
bench/RESULTS.md      latest benchmark results
tools/ma.gdb          GDB commands: ma-heap, ma-check, ma-chunk
```

## Design

The design follows dlmalloc/ptmalloc. It was written from scratch, sized for
reading in one sitting.

### Chunks and boundary tags

Every block is a 16-byte-aligned *chunk* with a one-word header. A free chunk
also stores its size in a footer, in the first word of the next chunk:

```
 chunk ->  prev_size   footer of the previous chunk (valid only if it is free)
           head        size | INUSE | PREV_INUSE | MMAPPED | arena id (bits 56-63)
 mem   ->  fd, bk      free-list links (free chunks only)
           payload...
 next  ->  prev_size   <- this chunk's footer when free, last 8 payload bytes when in use
```

Because the footer overlaps the next chunk's header, an in-use chunk costs only
**8 bytes of overhead**. The `PREV_INUSE` bit tells `free()` whether it can
merge backwards (read `prev_size`, jump to the previous chunk) without
searching. Coalescing is eager, so two free chunks are never adjacent.

### Segregated free lists

There are 186 bins:

* **62 exact-size small bins** for chunks of 32-1008 bytes, in 16-byte steps.
  A hit is O(1) and never needs a split.
* **124 large bins**, four per power of two from 1 KiB up. Within its own bin
  a large request does a bounded best-fit scan (at most 32 entries).

A 192-bit **bitmap** of non-empty bins lets a miss jump straight to the next
bin that can satisfy the request, using `ctz`. Any chunk there is big enough.
If the leftover is at least 32 bytes, the chunk is **split** and the remainder
goes back to its bin. If no bin can serve the request, it is carved from the
arena's **top chunk** (the wilderness at the end of the heap).

### Getting memory from the OS

* **Arena 0** (the main thread's arena) grows with `sbrk`, in steps of at least
  128 KiB. If the new break is contiguous, the top chunk just grows.
  Otherwise, for example when another library moved the break, a new segment
  starts.
* **Other arenas** grow with 1 MiB+ `mmap` segments.
* Every segment ends in a zero-size, in-use **fence** chunk, so coalescing
  never runs off the end. Segments that aren't contiguous are handled safely.
* Requests of **128 KiB or more** get their own `mmap`. `realloc` on them uses
  `mremap`, so large buffers grow without copying.
* **Trimming:** when the top chunk grows past 256 KiB, the tail goes back to
  the OS (`sbrk(-n)` or a partial `munmap`). An mmap segment that becomes
  entirely free is unmapped.

### Thread safety: per-arena mutexes

* There are up to 64 arenas, one per CPU by default (`MA_ARENAS=n` overrides
  this). Each arena has its own `pthread_mutex`, bins, top chunk and segments.
* A thread is bound to an arena round-robin on its first allocation. That
  binding lives in an initial-exec TLS variable, which is safe under
  `LD_PRELOAD`.
* If its arena is contended (`trylock` fails), the thread tries the other
  arenas and moves to the first free one. Threads spread out on their own
  under contention.
* Every chunk records its owning arena in its header, so a **cross-thread
  `free`** locks the owner's arena directly. There is no global lock anywhere
  on the allocation path.
* `pthread_atfork` handlers hold all arena locks across `fork()`, so a child
  never inherits a heap that is half-modified.

### Per-thread cache (tcache)

In front of the arenas, each thread keeps a small LIFO cache of freed small
chunks (32-1008 bytes): up to 8 per size class, in initial-exec TLS.

* **Fast paths.** A cached `malloc` or `free` is a few pointer operations and
  takes no lock.
* **Refill.** On a cache miss the thread takes its arena lock anyway. While it
  holds the lock, it also moves up to 4 more chunks of the same size from the
  exact bin into the cache.
* **Accounting.** Cached chunks keep their in-use bit. The arenas, the byte
  counters and `ma_check` see them as allocated, so none of the arena
  invariants change.
* **Double-free detection.** A cached chunk's `bk` word holds a per-process
  cookie. Freeing a chunk that carries the cookie triggers a scan of that
  cache bin, and a real double free aborts with
  `double free detected in tcache`.
* **Thread exit.** A pthread-key destructor drains the cache when its thread
  exits. `ma_tcache_flush()` drains the calling thread's cache on demand.
* **Switch.** `MA_TCACHE=0` turns the cache off, which the benchmarks use for
  comparison.

### realloc, memalign, calloc

* **realloc** tries these in order:
  1. Shrink in place, freeing the tail.
  2. Grow into the top chunk.
  3. Absorb a free neighbour.
  4. Fall back to malloc + copy + free.
* **memalign** over-allocates, then carves an aligned chunk out of the middle
  and frees the leading and trailing slack through the normal coalescing path.
* **calloc** checks for overflow and skips the `memset` on fresh `mmap` pages.

### Robustness

`free()` aborts with a diagnostic on obvious double frees, misaligned
pointers, a bad arena id or a corrupted `prev_size`, as glibc does.

## Debugging support

* **`ma_check()`** walks every arena and segment and checks every invariant:
  * alignment and sizes
  * `PREV_INUSE` matches the neighbour's state
  * no adjacent free chunks
  * footers match sizes
  * fences are intact
  * the top chunk is at the end of the current segment
  * every free chunk is in the right bin, with consistent links and bitmap
  * byte counters match a full heap walk

  The tests call it after every phase and every few thousand random
  operations.
* **`ma_dump(fd)`** prints arenas, segments and chunks without using stdio, so
  it is safe to call while arena locks are held.
* **GDB**: `tools/ma.gdb` adds `ma-heap`, `ma-check` and `ma-chunk PTR`:

  ```
  $ gdb -q -x tools/ma.gdb ./build/demo
  (gdb) break demo_checkpoint
  (gdb) run
  (gdb) up
  (gdb) ma-chunk a
  chunk 0x555555559020 size=112 inuse=1 prev_inuse=1 mmapped=0 arena=0
  (gdb) ma-check
  $1 = 0
  ```

  `./build/demo` steps through allocate / free / coalesce / split and dumps
  the heap at each step:

  ```
  -- a and c freed: two holes --
      0x...020      112 free
      0x...090      208 used
      0x...160      320 free
      0x...2a0       32 used
      0x...2c0   130352 top
  -- b freed: a+b+c coalesced into one chunk --
      0x...020      640 free
      0x...2a0       32 used
      0x...2c0   130352 top
  ```

* **Valgrind**: with `-DMA_VALGRIND`, the allocator describes its blocks to
  memcheck through client requests (`MALLOCLIKE_BLOCK`/`FREELIKE_BLOCK`).
  Everything obtained from the OS starts as NOACCESS, so memcheck catches
  overflows into chunk slack, use-after-free and leaks on blocks from `ma`,
  exactly as it does for glibc malloc. `make valgrind` runs the full test
  suite under memcheck with zero errors. It then runs `tests/leak.c`, where
  memcheck correctly reports the planted one-byte overflow, the use-after-free
  and the 64-byte leak.

  One subtlety came up while building this. Valgrind 3.22 defaults to
  `--partial-loads-ok=yes`, so an 8-byte metadata read that is half
  freed-block and half fresh `brk` memory loads as *undefined*. Marking whole
  OS regions NOACCESS up front fixes this.

## Build, test, benchmark

You need Linux, gcc and make. Valgrind and gdb are optional. On Windows, the
project was developed in an `ubuntu:24.04` Docker container.

```bash
make            # build/libma.a, build/libma.so, tests, bench, demo
make test       # unit + stress + 8-thread cross-free + fork + no-sbrk + LD_PRELOAD tests
make valgrind   # memcheck on the test suite, then on the planted-bug program
make bench      # 5 runs of every benchmark -> bench/results.csv, bench/RESULTS.md
LD_PRELOAD=$PWD/build/libma.so python3 -c "print('hello from ma')"
```

The test suite covers:

* alignment and odd sizes
* `malloc(0)` and overflowing `calloc`
* exact coalescing and splitting addresses
* in-place `realloc` (shrink, grow into top, absorb neighbour) and `mremap`
* `memalign` from 32 B to 64 KiB, including mmapped chunks with offsets
* trimming
* 400k random operations with content verification and `ma_check`
* 8 threads x 300k operations with blocks freed by other threads, with 12
  arenas and with 1
* `fork`
* a forced mmap-only mode (`MA_NO_SBRK=1`)
* the thread cache: LIFO reuse, accounting, draining on thread exit, and
  aborting on a double free
* the whole suite again with the cache off (`MA_TCACHE=0`)

## Results

Median of 5 runs on 12 vCPUs (i5-13420H, WSL2 kernel 6.6), gcc 13.3,
glibc 2.39. Full tables: [bench/RESULTS.md](bench/RESULTS.md). Benchmark
bookkeeping comes straight from `mmap`, so neither allocator pays for it.
Absolute numbers move with host load, so compare ratios from the same run.

| workload (Mops/s) | glibc | ma | ma / glibc |
|---|---:|---:|---:|
| small blocks, 1 thread | 39.5 | 49.1 | **1.24x** |
| mixed sizes, 1 thread | 13.0 | 18.2 | **1.40x** |
| realloc growth, 1 thread | 0.7 | 1.5 | **2.23x** |
| mixed, 8 threads (private) | 82.3 | 100.4 | **1.22x** |
| producer→consumer, 2 threads | 3.1 | 8.4 | **2.69x** |
| producer→consumer, 8 threads | 8.9 | 24.4 | **2.75x** |

What each layer contributes, measured on `ma` itself:

* **Per-thread cache** (`MA_TCACHE=0` compared with the default):
  * **2.3x** on single-thread small blocks
  * 1.3-1.5x on mixed and multithreaded workloads
  * 1.33x on the 8-thread producer/consumer workload
* **Per-arena locks** (`MA_ARENAS=1` compared with the default):
  * **5.1x at 2 threads, 21x at 4, 50x at 8** on private workloads
  * 8.6x on the 8-thread producer/consumer workload

Fragmentation trace: the heap fills to 64 MiB live, frees random blocks down
to 16 MiB, and repeats 20 times with a size distribution that shifts each
round.

| | glibc | ma | ma, no tcache |
|---|---:|---:|---:|
| peak footprint / live bytes | 1.108 | 1.109 | 1.109 |
| peak RSS (MiB) | 78.6 | 78.8 | 78.6 |
| footprint after freeing everything | 70.9 MiB | 71.0 MiB | **0.13 MiB** |
| trace run time | 1.69 s | 1.43 s | 1.29 s |

**What the numbers say:**

* **Throughput:** with the cache, `ma` beats glibc on every workload here.
  * Small-block speed is about the same as glibc's tcache, plus a 1.24x edge.
  * Mixed sizes are 1.4x: the bitmap bin search and eager coalescing pay off
    once requests miss the cache.
  * Cross-thread frees are about 2.7x: a remote free costs one lock on the
    owner's arena.
* **Fragmentation:** `ma` matches glibc almost exactly. Both are best-fit
  boundary-tag allocators.
* **Releasing memory:** the cache costs this. Chunks parked in the main
  thread's cache sit right below the top chunk and stop the trim, so like
  glibc, `ma` keeps its 71 MiB after the heap empties.
  * With the cache off, eager coalescing and trimming return all but
    0.13 MiB.
  * Calling `ma_tcache_flush()` gets the same result while keeping the cache
    on.

## Limitations / next steps

* **Trimming and the cache.** Cached chunks can pin the top of the heap. A
  smarter policy would skip caching chunks that border the top chunk, or
  drain the cache when the top chunk could otherwise be trimmed.
* **Double-free detection** only scans the freeing thread's own cache. A
  chunk freed twice from two different threads is not caught, a limitation
  glibc shares.
* **madvise.** Free chunks inside the heap are never
  `madvise(MADV_DONTNEED)`'d. Only the top chunk and empty mmap segments go
  back to the OS.
* **sbrk segments.** An emptied sbrk segment that is no longer at the end of
  the break cannot be released.
* **ThreadSanitizer.** `do_free` reads the header's `INUSE` bit before taking
  the lock, as a cheap sanity check. That is a benign race, but
  ThreadSanitizer would flag it.
