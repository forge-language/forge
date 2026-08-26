#!/usr/bin/env bash
# Advanced Forge HTTP benchmarks: sendfile, io_uring, TLS, routing
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build"
RESULTS="${ROOT}/benchmark/advanced_results.txt"
CPU_COUNT="$(nproc 2>/dev/null || echo 1)"
REQUESTS=1000000
CONCURRENCY=2500
TLS_DIR="${ROOT}/benchmark/tls"

SENDFILE_BIN="${BUILD}/bin/bench_sendfile_server"
URING_BIN="${BUILD}/bin/bench_uring_server"
TLS_BIN="${BUILD}/bin/bench_tls_server"
ROUTING_BIN="${BUILD}/bin/bench_routing_server"
MT_BIN="${BUILD}/bin/bench_server"

FORGE_PORT=19080
SENDFILE_PORT=19086
URING_PORT=19087
TLS_PORT=19088
ROUTING_PORT=19089

mkdir -p "$(dirname "$RESULTS")" "$TLS_DIR"

tune_for_benchmark() {
    if ulimit -n 1048576 2>/dev/null; then
        :
    elif ulimit -n 65536 2>/dev/null; then
        :
    else
        ulimit -n 4096 2>/dev/null || true
    fi
}

ensure_tls_certs() {
    if [[ -f "$TLS_DIR/cert.pem" && -f "$TLS_DIR/key.pem" ]]; then
        return
    fi
    openssl ecparam -genkey -name prime256v1 -out "$TLS_DIR/key.pem" 2>/dev/null
    openssl req -new -x509 -key "$TLS_DIR/key.pem" -out "$TLS_DIR/cert.pem" \
        -days 365 -subj "/CN=localhost" 2>/dev/null
}

run_load() {
    local url="$1"
    local paths="${2:-}"
    python3 - "$url" "$REQUESTS" "$CONCURRENCY" "$paths" <<'PY'
import socket, sys, time, random
from concurrent.futures import ThreadPoolExecutor, as_completed
from urllib.parse import urlparse

url, n, c, paths_csv = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
paths = [p for p in paths_csv.split(",") if p] if paths_csv else ["/"]
parsed = urlparse(url)
host = parsed.hostname or "127.0.0.1"
port = parsed.port or 80
scheme = parsed.scheme or "http"
is_tls = scheme == "https"

def make_req(path: str) -> bytes:
    return (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        f"Connection: close\r\n\r\n"
    ).encode()

ok = err = 0
start = time.perf_counter()
batch = 20000

def one(_):
    path = random.choice(paths)
    req = make_req(path)
    last_err = None
    for attempt in range(5):
        try:
            s = socket.create_connection((host, port), timeout=30)
            if is_tls:
                import ssl
                ctx = ssl.create_default_context()
                ctx.check_hostname = False
                ctx.verify_mode = ssl.CERT_NONE
                s = ctx.wrap_socket(s, server_hostname=host)
            s.sendall(req)
            while s.recv(8192):
                pass
            s.close()
            return 1
        except OSError as e:
            last_err = e
            time.sleep(0.001 * (attempt + 1))
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

bench_one() {
    local name="$1" port="$2" bin="$3" url_path="${4:-/}" paths="${5:-}"
    echo "=== $name (port $port) ==="
    if [[ ! -x "$bin" ]]; then
        echo "Missing binary: $bin" >&2
        echo
        return 1
    fi
    if command -v taskset >/dev/null 2>&1; then
        taskset -c "0-$((CPU_COUNT - 1))" "$bin" &
    else
        "$bin" &
    fi
    local pid=$!
    local ready=0
    local scheme="http"
    [[ "$name" == *TLS* ]] && scheme="https"
    for _ in $(seq 1 20); do
        if [[ "$scheme" == "https" ]]; then
            if curl -skf "${scheme}://127.0.0.1:${port}${url_path}" >/dev/null 2>&1; then
                ready=1
                break
            fi
        elif curl -sf "http://127.0.0.1:${port}${url_path}" >/dev/null; then
            ready=1
            break
        fi
        sleep 1
    done
    if [[ "$ready" -ne 1 ]]; then
        echo "Server failed to start" >&2
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        return 1
    fi
    run_load "${scheme}://127.0.0.1:${port}/" "$paths"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    fuser -k "${port}/tcp" 2>/dev/null || true
    sleep 1
    echo
}

ROUTING_PATHS="/,/api/health,/api/users,/api/users/1,/api/posts,/api/metrics,/api/version,/static/app.js,/static/style.css"

{
    tune_for_benchmark
    ensure_tls_certs

    echo "Forge Advanced HTTP Benchmark"
    echo "Date: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "Host: $(uname -srm)"
    echo "CPU cores: $CPU_COUNT"
    echo "Requests: $REQUESTS, Concurrency: $CONCURRENCY"
    echo

    bench_one "Epoll MT (baseline)" "$FORGE_PORT" "$MT_BIN"
    bench_one "Sendfile MT (memfd)" "$SENDFILE_PORT" "$SENDFILE_BIN"
    bench_one "io_uring MT" "$URING_PORT" "$URING_BIN"
    bench_one "TLS MT (OpenSSL)" "$TLS_PORT" "$TLS_BIN"
    bench_one "Routing MT (9 JSON routes)" "$ROUTING_PORT" "$ROUTING_BIN" "/" "$ROUTING_PATHS"
} | tee "$RESULTS"

echo "Results saved to $RESULTS"
