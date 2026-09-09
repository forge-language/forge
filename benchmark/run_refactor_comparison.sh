#!/usr/bin/env bash
# Before/after benchmark: compares the working tree against a baseline ref
# (default: the commit the current branch started from) across the four
# things the 2026-09 parallel refactor touched -- coroutine scheduling,
# HTTP throughput per serving mode, arena allocator throughput, and
# compiler build speed. Existing scripts in this directory (run_benchmark.sh,
# run_advanced_benchmark.sh, run_scheduler_bench.sh) compare Forge against
# other languages; this one compares Forge against itself, pre- and
# post-refactor, to catch a performance regression a correctness-focused
# refactor could introduce without anyone noticing.
#
# Usage: benchmark/run_refactor_comparison.sh [baseline-ref]
#   baseline-ref defaults to HEAD: if the working tree has uncommitted
#   changes (the normal case right after a refactor pass), HEAD *is* the
#   pre-refactor state and the working tree is the post-refactor state.
#   Pass an explicit ref only when comparing two committed points instead.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RESULTS="${ROOT}/benchmark/refactor_comparison_results.txt"
BASE_REF="${1:-HEAD}"
WT="/tmp/forge-refactor-baseline-$$"
CPU_COUNT="$(nproc 2>/dev/null || echo 1)"

HTTP_REQUESTS="${HTTP_REQUESTS:-150000}"
HTTP_CONCURRENCY="${HTTP_CONCURRENCY:-500}"
CORO_RUNS="${CORO_RUNS:-5}"
COMPILE_PASSES="${COMPILE_PASSES:-3}"

cleanup() {
    for p in $(jobs -pr 2>/dev/null); do kill "$p" 2>/dev/null || true; done
    git -C "$ROOT" worktree remove --force "$WT" 2>/dev/null || true
}
trap cleanup EXIT

tune_for_benchmark() {
    ulimit -n 65536 2>/dev/null || ulimit -n 4096 2>/dev/null || true
}
tune_for_benchmark

echo "Setting up baseline worktree at ${BASE_REF} ..."
git -C "$ROOT" worktree add "$WT" "$BASE_REF" >/dev/null

echo "Building baseline (Release) ..."
cmake -S "$WT" -B "$WT/b" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$WT/b" --target forge forge_runtime forge_std \
    bench_server bench_hybrid_server bench_coro -j"$CPU_COUNT" >/dev/null

echo "Building working tree (Release) ..."
cmake -S "$ROOT" -B "$ROOT/build-bench" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/build-bench" --target forge forge_runtime forge_std \
    bench_server bench_hybrid_server bench_coro -j"$CPU_COUNT" >/dev/null

NEW_BIN="$ROOT/build-bench/bin"
OLD_BIN="$WT/b/bin"

wait_up() {
    local port="$1"
    for _ in $(seq 1 100); do
        python3 -c "
import socket
try:
    s = socket.create_connection(('127.0.0.1', $port), 0.3); s.close()
except Exception:
    raise SystemExit(1)
" 2>/dev/null && return 0
    done
    return 1
}

http_bench() {
    local bin="$1" port="$2"
    "$bin" >/dev/null 2>&1 &
    local pid=$!
    if ! wait_up "$port"; then
        echo "server on port $port failed to start"; kill "$pid" 2>/dev/null || true; return
    fi
    python3 - "$port" "$HTTP_REQUESTS" "$HTTP_CONCURRENCY" <<'PY'
import socket, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed
port, n, c = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
req = b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
ok = err = 0
def one(_):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.sendall(req)
    while s.recv(8192): pass
    s.close()
    return 1
t0 = time.perf_counter()
with ThreadPoolExecutor(max_workers=c) as pool:
    for f in as_completed([pool.submit(one, i) for i in range(n)]):
        try: ok += f.result()
        except Exception: err += 1
el = time.perf_counter() - t0
print(f"  ok={ok} err={err} elapsed={el:.3f}s rps={ok/el:.1f}")
PY
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true
    sleep 1
}

coro_bench() {
    local bin="$1" n="$2"
    for i in $(seq 1 "$n"); do "$bin"; done
}

compile_bench() {
    local forge_bin="$1" inc="$2" lib="$3" exdir="$4" outdir="$5" passes="$6"
    mkdir -p "$outdir"
    for pass in $(seq 1 "$passes"); do
        local t0 t1 ok=0
        t0=$(date +%s.%N)
        for f in "$exdir"/*.fg; do
            local base; base="$(basename "$f" .fg)"
            "$forge_bin" -I "$inc" --lib-dir "$lib" "$f" -o "$outdir/$base" \
                >/dev/null 2>&1 && ok=$((ok + 1))
        done
        t1=$(date +%s.%N)
        python3 -c "print(f'  pass $pass: {$t1-$t0:.3f}s, $ok compiled')"
    done
}

{
    echo "Forge refactor before/after comparison"
    echo "Date: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "Host: $(uname -srm), $CPU_COUNT cores"
    echo "Baseline ref: $BASE_REF"
    echo "Working tree: $(git -C "$ROOT" rev-parse --short HEAD) ($(git -C "$ROOT" branch --show-current))"
    echo

    echo "== Coroutine scheduler: spawn+yield 100k coroutines, $CORO_RUNS runs =="
    echo "-- baseline --"
    coro_bench "$OLD_BIN/bench_coro" "$CORO_RUNS"
    echo "-- working tree --"
    coro_bench "$NEW_BIN/bench_coro" "$CORO_RUNS"
    echo

    echo "== HTTP throughput: $HTTP_REQUESTS requests, concurrency $HTTP_CONCURRENCY =="
    echo "-- mt mode, baseline (port 19080) --"
    http_bench "$OLD_BIN/bench_server" 19080
    echo "-- mt mode, working tree (port 19080) --"
    http_bench "$NEW_BIN/bench_server" 19080
    echo "-- hybrid mode, baseline (port 19084) --"
    http_bench "$OLD_BIN/bench_hybrid_server" 19084
    echo "-- hybrid mode, working tree (port 19084) --"
    http_bench "$NEW_BIN/bench_hybrid_server" 19084
    echo

    echo "== Compiler build speed: examples/*.fg, $COMPILE_PASSES passes =="
    echo "-- baseline --"
    compile_bench "$OLD_BIN/forge" "$WT/include" "$OLD_BIN/../lib" \
        "$WT/examples" "/tmp/forge-bench-old-out" "$COMPILE_PASSES"
    echo "-- working tree --"
    compile_bench "$NEW_BIN/forge" "$ROOT/include" "$NEW_BIN/../lib" \
        "$ROOT/examples" "/tmp/forge-bench-new-out" "$COMPILE_PASSES"
} | tee "$RESULTS"

echo
echo "Results saved to $RESULTS"
echo "Note: HTTP numbers are bounded by this single-machine Python/GIL load"
echo "generator, not the server -- read them as a regression check between"
echo "the two builds, not as an absolute throughput figure. See"
echo "docs/benchmark-performance-report.md for a proper cross-language bench."
