# Benchmark results

Median of 5 runs. Environment: 12 vCPUs (13th Gen Intel(R) Core(TM) i5-13420H), Linux 6.6.87.2-microsoft-standard-WSL2, gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0, ldd (Ubuntu GLIBC 2.39-0ubuntu8.9) 2.39.

Regenerate with `make bench` (or `REPS=n ./scripts/run_bench.sh`).

## Throughput (million ops/s, higher is better)

| workload | threads | glibc | ma | ma / glibc |
|---|---:|---:|---:|---:|
| small | 1 | 51.8 | 23.5 | 0.45x |
| mixed | 1 | 14.9 | 16.3 | 1.09x |
| realloc | 1 | 1.0 | 1.1 | 1.12x |
| threads | 1 | 14.6 | 15.3 | 1.05x |
| threads | 2 | 29.3 | 27.6 | 0.94x |
| threads | 4 | 50.3 | 47.1 | 0.94x |
| threads | 8 | 87.4 | 77.8 | 0.89x |
| xthread | 2 | 2.6 | 10.5 | 3.98x |
| xthread | 4 | 5.2 | 17.2 | 3.32x |
| xthread | 8 | 9.1 | 22.0 | 2.41x |

## Per-arena locks vs a single global lock (million ops/s)

| workload | threads | ma, 1 arena | ma, per-CPU arenas | speedup |
|---|---:|---:|---:|---:|
| threads | 1 | 16.0 | 15.3 | 0.96x |
| threads | 2 | 4.7 | 27.6 | 5.87x |
| threads | 4 | 2.5 | 47.1 | 18.81x |
| threads | 8 | 1.3 | 77.8 | 58.17x |
| xthread | 2 | 3.7 | 10.5 | 2.84x |
| xthread | 4 | 4.6 | 17.2 | 3.78x |
| xthread | 8 | 2.5 | 22.0 | 8.90x |

## Fragmentation trace (64 MiB live peak, lower is better)

| metric | glibc | ma |
|---|---:|---:|
| peak footprint (MiB from OS) | 70.9 | 71.0 |
| footprint / live bytes, at peaks | 1.108 | 1.109 |
| footprint / live bytes, after frees | 4.431 | 4.438 |
| peak RSS (MiB) | 78.5 | 78.8 |
| RSS / live bytes, at peaks | 1.227 | 1.230 |
| RSS / live bytes, after frees | 4.907 | 4.922 |
| footprint after freeing everything (MiB) | 70.90 | 0.13 |
| trace run time (s) | 1.59 | 1.13 |
