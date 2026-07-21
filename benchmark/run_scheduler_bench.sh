#!/usr/bin/env bash
# Benchmark coroutine scheduler + compare HTTP mt vs hybrid
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build"
CORO_BIN="${BUILD}/bin/bench_coro"
MT_BIN="${BUILD}/bin/bench_server"
HYBRID_BIN="${BUILD}/bin/bench_hybrid_server"
RESULTS="${ROOT}/benchmark/scheduler_results.txt"
CPU_COUNT="$(nproc 2>/dev/null || echo 1)"
REQUESTS=200000
CONCURRENCY=500

mkdir -p "$(dirname "$RESULTS")"

run_load() {
    local url="$1"
    python3 - "$url" "$REQUESTS" "$CONCURRENCY" <<'PY'
import socket, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed
from urllib.parse import urlparse

url, n, c = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
parsed = urlparse(url)
host = parsed.hostname or "127.0.0.1"
port = parsed.port or 80
req = (
    f"GET {parsed.path or '/'} HTTP/1.1\r\n"
    f"Host: {host}\r\n"
    f"Connection: close\r\n\r\n"
).encode()

ok = err = 0
start = time.perf_counter()

def one(_):
    s = socket.create_connection((host, port), timeout=30)
    s.sendall(req)
    while s.recv(8192):
        pass
    s.close()
    return 1

with ThreadPoolExecutor(max_workers=c) as pool:
    futs = [pool.submit(one, i) for i in range(n)]
    for f in as_completed(futs):
        try:
            ok += f.result()
        except Exception:
            err += 1

elapsed = time.perf_counter() - start
rps = ok / elapsed if elapsed > 0 else 0
print(f"Requests:      {ok}")
print(f"Errors:        {err}")
print(f"Duration:      {elapsed:.4f}s")
print(f"Requests/sec:  {rps:.2f}")
PY
}

bench_http() {
    local name="$1" port="$2" bin="$3"
    echo "=== $name (port $port) ==="
    "$bin" &
    local pid=$!
    local ready=0
    for _ in $(seq 1 15); do
        if curl -sf "http://127.0.0.1:${port}/" >/dev/null; then
            ready=1
            break
        fi
        sleep 1
    done
    if [[ "$ready" -ne 1 ]]; then
        echo "Server failed to start" >&2
        kill "$pid" 2>/dev/null || true
        return 1
    fi
    run_load "http://127.0.0.1:${port}/"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    fuser -k "${port}/tcp" 2>/dev/null || true
    sleep 1
    echo
}

{
    echo "Forge Hybrid Scheduler Benchmark"
    echo "Date: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "Host: $(uname -srm)"
    echo "CPU cores: $CPU_COUNT"
    echo

    echo "=== Coroutine scheduler (bench_coro.fg) ==="
    if [[ -x "$CORO_BIN" ]]; then
        "$CORO_BIN"
    else
        echo "Missing $CORO_BIN"
    fi
    echo

    echo "HTTP compare: $REQUESTS requests, concurrency $CONCURRENCY"
    echo
    bench_http "HTTP mt (REUSEPORT pthread)" 19080 "$MT_BIN"
    bench_http "HTTP hybrid (REUSEPORT + scheduler pool)" 19084 "$HYBRID_BIN"
} | tee "$RESULTS"

echo "Results saved to $RESULTS"
