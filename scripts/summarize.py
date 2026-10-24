#!/usr/bin/env python3
"""Turn bench/results.csv into Markdown tables of medians."""
import csv
import platform
import statistics
import subprocess
import sys
from collections import defaultdict

rows = list(csv.DictReader(open(sys.argv[1])))
vals = defaultdict(list)
for r in rows:
    vals[(r["allocator"], r["workload"], int(r["threads"]), r["metric"])].append(float(r["value"]))
med = {k: statistics.median(v) for k, v in vals.items()}
reps = max(len(v) for v in vals.values())


def get(alloc, wl, t, metric):
    return med.get((alloc, wl, t, metric))


def sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()
    except OSError:
        return "?"


print("# Benchmark results\n")
cpu = sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2").strip()
print(f"Median of {reps} runs. Environment: {sh('nproc')} vCPUs ({cpu}), "
      f"Linux {platform.release()}, {sh('gcc --version | head -1')}, "
      f"{sh('ldd --version | head -1')}.\n")
print("Regenerate with `make bench` (or `REPS=n ./scripts/run_bench.sh`).\n")

print("## Throughput (million ops/s, higher is better)\n")
print("| workload | threads | glibc | ma | ma / glibc |")
print("|---|---:|---:|---:|---:|")
order = [("small", 1), ("mixed", 1), ("realloc", 1)] + \
        [("threads", t) for t in (1, 2, 4, 8)] + [("xthread", t) for t in (2, 4, 8)]
for wl, t in order:
    g, m = get("glibc", wl, t, "mops_per_sec"), get("ma", wl, t, "mops_per_sec")
    if g and m:
        print(f"| {wl} | {t} | {g:.1f} | {m:.1f} | {m / g:.2f}x |")

print("\n## Effect of the per-thread cache (million ops/s)\n")
print("| workload | threads | ma, no tcache | ma, tcache | speedup |")
print("|---|---:|---:|---:|---:|")
for wl, t in order:
    off, on = get("ma-notcache", wl, t, "mops_per_sec"), get("ma", wl, t, "mops_per_sec")
    if off and on:
        print(f"| {wl} | {t} | {off:.1f} | {on:.1f} | {on / off:.2f}x |")

print("\n## Per-arena locks vs a single global lock (million ops/s)\n")
print("| workload | threads | ma, 1 arena | ma, per-CPU arenas | speedup |")
print("|---|---:|---:|---:|---:|")
for wl, t in [("threads", t) for t in (1, 2, 4, 8)] + [("xthread", t) for t in (2, 4, 8)]:
    one, many = get("ma-1arena", wl, t, "mops_per_sec"), get("ma", wl, t, "mops_per_sec")
    if one and many:
        print(f"| {wl} | {t} | {one:.1f} | {many:.1f} | {many / one:.2f}x |")

print("\n## Fragmentation trace (64 MiB live peak, lower is better)\n")
print("| metric | glibc | ma | ma, no tcache |")
print("|---|---:|---:|---:|")
labels = [
    ("peak_footprint_mib", "peak footprint (MiB from OS)", "{:.1f}"),
    ("footprint_over_live_at_peaks", "footprint / live bytes, at peaks", "{:.3f}"),
    ("footprint_over_live_at_troughs", "footprint / live bytes, after frees", "{:.3f}"),
    ("peak_rss_mib", "peak RSS (MiB)", "{:.1f}"),
    ("rss_over_live_at_peaks", "RSS / live bytes, at peaks", "{:.3f}"),
    ("rss_over_live_at_troughs", "RSS / live bytes, after frees", "{:.3f}"),
    ("footprint_after_free_all_mib", "footprint after freeing everything (MiB)", "{:.2f}"),
    ("seconds", "trace run time (s)", "{:.2f}"),
]
for key, label, fmt in labels:
    g, m, n = (get(a, "frag", 1, key) for a in ("glibc", "ma", "ma-notcache"))
    if g is not None and m is not None:
        nt = fmt.format(n) if n is not None else "-"
        print(f"| {label} | {fmt.format(g)} | {fmt.format(m)} | {nt} |")
