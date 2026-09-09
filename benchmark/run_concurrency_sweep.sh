#!/usr/bin/env bash
# Concurrency-scaling sweep across the 5 existing Forge HTTP serving modes
# (epoll baseline, sendfile, io_uring, TLS, routing). Reuses the same
# binaries and Python load generator as run_advanced_benchmark.sh; adds no
# new server code, only measures throughput/error behavior across a range
# of concurrency levels instead of a single fixed one.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build"
RESULTS="${ROOT}/benchmark/concurrency_sweep_results.txt"
CPU_COUNT="$(nproc 2>/dev/null || echo 1)"
REQUESTS="${REQUESTS:-200000}"
CONCURRENCY_LEVELS=(100 500 1000 2500 5000 10000)
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

# Same load generator as run_advanced_benchmark.sh, parameterized by
# concurrency (that script hardcodes it into the heredoc; here it's an
# argument so one process of this script can sweep multiple levels).
run_load() {
    local url="$1" concurrency="$2" paths="${3:-}"
    python3 - "$url" "$REQUESTS" "$concurrency" "$paths" <<'PY'
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

ok = err = retries = 0
start = time.perf_counter()
batch = min(20000, max(c, 1000))

# Returns (1, retries_used). A request that failed once and succeeded on a
# later attempt still counts as ok, so `errors` alone would hide transient
# connection refusals (accept-backlog overflow) entirely -- hence the
# separate retry count in the output line.
def one(_):
    path = random.choice(paths)
    req = make_req(path)
    last_err = None
    for attempt in range(5):
        try:
            s = socket.create_connection((host, port), timeout=30)
            if is_tls:
                import ssl
                # Self-signed benchmark cert: verification is deliberately
                # off. Loopback-only, measures TLS record/handshake cost,
                # never used against a real endpoint.
                ctx = ssl.create_default_context()
                ctx.check_hostname = False
                ctx.verify_mode = ssl.CERT_NONE
                s = ctx.wrap_socket(s, server_hostname=host)
            s.sendall(req)
            while s.recv(8192):
                pass
            s.close()
            return 1, attempt
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
                got, used = f.result()
                ok += got
                retries += used
            except Exception:
                err += 1
    done += chunk

elapsed = time.perf_counter() - start
rps = ok / elapsed if elapsed > 0 else 0
print(f"concurrency={c} requests={ok} errors={err} retries={retries} "
      f"duration_s={elapsed:.4f} rps={rps:.2f}")
PY
}

wait_ready() {
    local scheme="$1" port="$2" url_path="${3:-/}"
    for _ in $(seq 1 20); do
        if [[ "$scheme" == "https" ]]; then
            curl -skf "${scheme}://127.0.0.1:${port}${url_path}" >/dev/null 2>&1 && return 0
        else
            curl -sf "http://127.0.0.1:${port}${url_path}" >/dev/null 2>&1 && return 0
        fi
        sleep 1
    done
    return 1
}

sweep_one() {
    local name="$1" port="$2" bin="$3" url_path="${4:-/}" paths="${5:-}"
    local scheme="http"
    [[ "$name" == *TLS* ]] && scheme="https"

    echo "=== $name (port $port) ==="
    # Skip rather than fail: under `set -e` a nonzero return here would abort
    # the whole sweep, losing the modes that *are* built. The skip is visible
    # in the results file, so it can't be mistaken for a passing run.
    if [[ ! -x "$bin" ]]; then
        echo "SKIPPED: missing binary $bin"
        echo
        return 0
    fi

    for c in "${CONCURRENCY_LEVELS[@]}"; do
        if command -v taskset >/dev/null 2>&1; then
            taskset -c "0-$((CPU_COUNT - 1))" "$bin" &
        else
            "$bin" &
        fi
        local pid=$!

        if ! wait_ready "$scheme" "$port" "$url_path"; then
            echo "  concurrency=$c: server failed to start" >&2
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            continue
        fi

        run_load "${scheme}://127.0.0.1:${port}/" "$c" "$paths"

        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        fuser -k "${port}/tcp" 2>/dev/null || true
        sleep 1
    done
    echo
}

ROUTING_PATHS="/,/api/health,/api/users,/api/users/1,/api/posts,/api/metrics,/api/version,/static/app.js,/static/style.css"

{
    tune_for_benchmark
    ensure_tls_certs

    echo "Forge HTTP Concurrency Sweep"
    echo "Date: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "Host: $(uname -srm)"
    echo "CPU cores: $CPU_COUNT"
    echo "Requests per level: $REQUESTS, Concurrency levels: ${CONCURRENCY_LEVELS[*]}"
    echo

    sweep_one "Epoll MT (baseline)" "$FORGE_PORT" "$MT_BIN"
    sweep_one "Sendfile MT (memfd)" "$SENDFILE_PORT" "$SENDFILE_BIN"
    sweep_one "io_uring MT" "$URING_PORT" "$URING_BIN"
    sweep_one "TLS MT (OpenSSL)" "$TLS_PORT" "$TLS_BIN"
    sweep_one "Routing MT (9 JSON routes)" "$ROUTING_PORT" "$ROUTING_BIN" "/" "$ROUTING_PATHS"
} | tee "$RESULTS"

echo "Results saved to $RESULTS"
