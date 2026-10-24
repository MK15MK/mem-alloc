#!/usr/bin/env bash
# Run every benchmark REPS times per allocator and summarise medians into
# bench/results.csv (raw) and bench/RESULTS.md (tables).
set -euo pipefail
cd "$(dirname "$0")/.."
REPS=${REPS:-5}
BIN=./build/bench
RAW=bench/results.csv

echo "allocator,workload,threads,metric,value" > "$RAW"
run() {  # label env... -- args...
    local label=$1; shift
    local envs=()
    while [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    for _ in $(seq "$REPS"); do
        env "${envs[@]}" "$BIN" "$@" | sed "s/^[^,]*,/$label,/" >> "$RAW"
    done
}

for alloc in glibc ma; do
    run "$alloc" X=1 -- "$alloc" small
    run "$alloc" X=1 -- "$alloc" mixed
    run "$alloc" X=1 -- "$alloc" realloc
    for t in 1 2 4 8; do run "$alloc" X=1 -- "$alloc" threads "$t"; done
    for t in 2 4 8; do run "$alloc" X=1 -- "$alloc" xthread "$t"; done
    run "$alloc" X=1 -- "$alloc" frag
done
# Same allocator without the per-thread cache: what does the tcache buy?
run "ma-notcache" MA_TCACHE=0 -- ma small
run "ma-notcache" MA_TCACHE=0 -- ma mixed
run "ma-notcache" MA_TCACHE=0 -- ma realloc
for t in 1 2 4 8; do run "ma-notcache" MA_TCACHE=0 -- ma threads "$t"; done
for t in 2 4 8; do run "ma-notcache" MA_TCACHE=0 -- ma xthread "$t"; done
run "ma-notcache" MA_TCACHE=0 -- ma frag
# Same allocator with a single arena: what do per-arena locks buy?
for t in 1 2 4 8; do run "ma-1arena" MA_ARENAS=1 -- ma threads "$t"; done
for t in 2 4 8; do run "ma-1arena" MA_ARENAS=1 -- ma xthread "$t"; done

python3 scripts/summarize.py "$RAW" > bench/RESULTS.md
cat bench/RESULTS.md
