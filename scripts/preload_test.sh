#!/usr/bin/env bash
# Run real programs with ma replacing the system allocator via LD_PRELOAD.
set -euo pipefail
cd "$(dirname "$0")/.."
LIB="$PWD/build/libma.so"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

run() {
    echo "preload: $*"
    LD_PRELOAD="$LIB" "$@" > "$TMP/out" 2>&1 || { cat "$TMP/out"; echo "FAILED: $*"; exit 1; }
}

run ls -laR /usr/include
run sort -R /etc/services
run python3 -c "import json, re; d = {str(i): list(range(i % 50)) for i in range(200000)}; s = json.dumps(d); assert json.loads(s) == d"
run gcc -O2 -c src/ma.c -Iinclude -o "$TMP/ma.o"
run bash -c 'for i in $(seq 1 200); do x=$(printf "%0${i}d" 0); done'
echo "preload: all programs ran correctly with libma.so"
