#!/bin/sh
# Usage: bench/run.sh <path-to-PoolAllocator-binary> [runs=5]
# Prints an environment header, discards one warmup run, then prints N raw runs
# and a min/median/max summary. Caller tees stdout to bench/results/.
set -eu

BIN=${1:?usage: bench/run.sh <path-to-PoolAllocator-binary> [runs=5]}
RUNS=${2:-5}
REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$(cd "$(dirname "$BIN")" && pwd)

# bench/results is excluded: the caller's tee creates the output file there first.
DIRTY=$(git -C "$REPO" status --porcelain -- . ':!bench/results' | wc -l | tr -d ' ')
if [ "$DIRTY" != 0 ]; then
  echo "abort: $DIRTY uncommitted change(s) in $REPO" >&2
  exit 1
fi

PREFIX=""
PIN="none"
if [ "$(uname)" = Darwin ]; then
  PIN="none (macOS)"
elif command -v taskset >/dev/null 2>&1; then
  PREFIX="taskset -c 2"
  PIN="taskset -c 2"
fi
command -v caffeinate >/dev/null 2>&1 && PREFIX="caffeinate -i $PREFIX"

echo "date: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo "commit: $(git -C "$REPO" rev-parse HEAD)"
echo "dirty files: $DIRTY"
echo "compiler: $(c++ --version | head -1)"
echo "cmake: $(cmake --version | head -1)"
echo "uname: $(uname -a)"
if [ "$(uname)" = Darwin ]; then
  echo "cpu: $(sysctl -n machdep.cpu.brand_string)"
  echo "power: $(pmset -g batt | head -1)"
else
  echo "cpu: $(lscpu | grep 'Model name' | sed 's/.*: *//')"
fi
grep -E '^CMAKE_BUILD_TYPE|^CMAKE_CXX_FLAGS_RELEASE' "$BUILD/CMakeCache.txt" || true
echo "pinning: $PIN"
echo "command: $0 $*"
echo

$PREFIX "$BIN" >/dev/null # warmup, discarded

OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
i=1
while [ "$i" -le "$RUNS" ]; do
  echo "=== run $i/$RUNS ==="
  $PREFIX "$BIN" | tee -a "$OUT"
  echo
  i=$((i + 1))
done

echo "=== summary over $RUNS runs: min / median / max ==="
# key = label before ':', value = first number after it.
grep -E '^  (new|pool) |speedup \(alloc\+free|guard cost' "$OUT" |
  awk -F: '{ split($2, f, " "); sub(/x$/, "", f[1]); print $1 "\t" f[1] }' |
  sort -t "$(printf '\t')" -k1,1 -k2,2g |
  awk -F'\t' '
    function flush() { if (n) printf "%-36s %8.2f %8.2f %8.2f\n", key ":", v[1], v[int((n + 1) / 2)], v[n] }
    $1 != key { flush(); key = $1; n = 0 }
    { v[++n] = $2 }
    END { flush() }'
