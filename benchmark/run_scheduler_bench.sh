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
REQUESTS=1000000
CONCURRENCY=2500

mkdir -p "$(dirname "$RESULTS")"

tune_for_benchmark() {
    if ulimit -n 1048576 2>/dev/null; then
        :
    elif ulimit -n 65536 2>/dev/null; then
        :
    else
        ulimit -n 4096 2>/dev/null || true
    fi
}

tune_for_benchmark

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
batch = 20000

def one(_):
    last_err = None
    for _attempt in range(5):
        try:
            s = socket.create_connection((host, port), timeout=30)
            s.sendall(req)
            while s.recv(8192):
                pass
            s.close()
            return 1
        except OSError as e:
            last_err = e
            time.sleep(0.001 * (_attempt + 1))
    raise last_err

done = 0
while done < n:
    chunk = min(batch, n - done)
    with ThreadPoolExecutor(max_workers=c) as pool:
        futs = [pool.submit(one, i) for i in range(chunk)]
        for f in as_completed(futs):
            try:
                ok += f.result()
            except Exception:
                err += 1
    done += chunk
    if done % 50000 == 0 or done == n:
        elapsed = time.perf_counter() - start
        rps = ok / elapsed if elapsed > 0 else 0
        print(f"Progress:      {done}/{n} ({rps:.0f} req/s, {err} errors)", flush=True)

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
    if command -v taskset >/dev/null 2>&1; then
        taskset -c "0-$((CPU_COUNT - 1))" "$bin" &
    else
        "$bin" &
    fi
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
