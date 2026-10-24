# Benchmark results

Median of 5 runs. Environment: 12 vCPUs (13th Gen Intel(R) Core(TM) i5-13420H), Linux 6.6.87.2-microsoft-standard-WSL2, gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0, ldd (Ubuntu GLIBC 2.39-0ubuntu8.9) 2.39.

Regenerate with `make bench` (or `REPS=n ./scripts/run_bench.sh`).

## Throughput (million ops/s, higher is better)

| workload | threads | glibc | ma | ma / glibc |
|---|---:|---:|---:|---:|
| small | 1 | 39.5 | 49.1 | 1.24x |
| mixed | 1 | 13.0 | 18.2 | 1.40x |
| realloc | 1 | 0.7 | 1.5 | 2.23x |
| threads | 1 | 12.5 | 16.1 | 1.29x |
| threads | 2 | 28.3 | 29.4 | 1.04x |
| threads | 4 | 50.1 | 59.3 | 1.18x |
| threads | 8 | 82.3 | 100.4 | 1.22x |
| xthread | 2 | 3.1 | 8.4 | 2.69x |
| xthread | 4 | 6.4 | 17.5 | 2.75x |
| xthread | 8 | 8.9 | 24.4 | 2.75x |

## Effect of the per-thread cache (million ops/s)

| workload | threads | ma, no tcache | ma, tcache | speedup |
|---|---:|---:|---:|---:|
| small | 1 | 21.1 | 49.1 | 2.32x |
| mixed | 1 | 13.9 | 18.2 | 1.31x |
| realloc | 1 | 0.9 | 1.5 | 1.62x |
| threads | 1 | 11.0 | 16.1 | 1.46x |
| threads | 2 | 22.1 | 29.4 | 1.33x |
| threads | 4 | 40.3 | 59.3 | 1.47x |
| threads | 8 | 73.3 | 100.4 | 1.37x |
| xthread | 2 | 8.5 | 8.4 | 0.98x |
| xthread | 4 | 12.9 | 17.5 | 1.35x |
| xthread | 8 | 18.4 | 24.4 | 1.33x |

## Per-arena locks vs a single global lock (million ops/s)

| workload | threads | ma, 1 arena | ma, per-CPU arenas | speedup |
|---|---:|---:|---:|---:|
| threads | 1 | 15.9 | 16.1 | 1.02x |
| threads | 2 | 5.8 | 29.4 | 5.06x |
| threads | 4 | 2.8 | 59.3 | 20.96x |
| threads | 8 | 2.0 | 100.4 | 49.89x |
| xthread | 2 | 3.4 | 8.4 | 2.48x |
| xthread | 4 | 4.2 | 17.5 | 4.21x |
| xthread | 8 | 2.8 | 24.4 | 8.61x |

## Fragmentation trace (64 MiB live peak, lower is better)

| metric | glibc | ma | ma, no tcache |
|---|---:|---:|---:|
| peak footprint (MiB from OS) | 70.9 | 71.0 | 71.0 |
| footprint / live bytes, at peaks | 1.108 | 1.109 | 1.109 |
| footprint / live bytes, after frees | 4.431 | 4.438 | 4.438 |
| peak RSS (MiB) | 78.6 | 78.8 | 78.6 |
| RSS / live bytes, at peaks | 1.228 | 1.230 | 1.228 |
| RSS / live bytes, after frees | 4.915 | 4.922 | 4.915 |
| footprint after freeing everything (MiB) | 70.90 | 71.00 | 0.13 |
| trace run time (s) | 1.69 | 1.43 | 1.29 |
