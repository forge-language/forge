#!/usr/bin/env bash
# Cross-language HTTP throughput comparison for Forge.
#
# Companion to run_refactor_comparison.sh, which compares Forge against
# itself before and after a refactor. This one compares Forge against the
# languages it competes with, so the numbers say something about the design
# rather than only about a diff.
#
# Fairness rules this harness enforces rather than assumes:
#   * Every server answers with the same 12-byte body and closes the
#     connection. The Forge HTTP stack hardcodes `Connection: close` and has
#     no keep-alive (stdlib/http.c), so connection-per-request is the only
#     shape all servers can be measured in. Every server is held to it.
#   * Before any server is timed, its response is probed and recorded byte
#     for byte (xlang/probe_wire.py). A server that sends extra headers shows
#     up in the results as a larger response instead of quietly looking
#     different for reasons unrelated to its runtime.
#   * A server whose response is wrong is reported as FAILED, never timed.
#   * The load generator is a compiled Go program (benchmark/loadgen). The
#     Python generator used elsewhere in this directory is GIL-bound at
#     roughly 8k rps, which is below what several of these servers can do
#     and would flatten them into a false tie.
#   * CPU-seconds and peak RSS are sampled across each server's whole
#     process tree (xlang/monitor.py), so throughput can be read per core
#     consumed. Raw rps rewards a server for burning 18 cores; rps per core
#     is the figure that compares runtimes.
#
# Usage: benchmark/run_crosslang_benchmark.sh [server-name ...]
#   With no arguments, runs every available server.
#   REQUESTS / CONCURRENCY / SUITE_RUNS override the defaults below.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH="$ROOT/benchmark"
RESULTS="${RESULTS:-$BENCH/crosslang_results.txt}"
JSONL="${JSONL:-$BENCH/crosslang_results.jsonl}"
LOADGEN="$BENCH/loadgen/loadgen"
MONITOR="$BENCH/xlang/monitor.py"
PROBE="$BENCH/xlang/probe_wire.py"
FORGE_BIN="$ROOT/build-xlang/bin"

REQUESTS="${REQUESTS:-300000}"
CONCURRENCY="${CONCURRENCY:-200}"
SUITE_RUNS="${SUITE_RUNS:-3}"

# This box has no thermal headroom. It idles at ~72 C and reaches 98-104 C
# under any sustained load, where it clocks down to roughly 1650 MHz against
# a 4500 MHz rated maximum. Measuring a cool machine is not an option here,
# and a suite that ignored this produced a monotonic decline inside every
# server -- each server's third run landed 20-25% under its first -- which
# reads as a software difference and is nothing of the kind.
#
# Uniform throttling is only a slower machine, and still a fair comparison.
# The damage comes from letting thermal state line up with server identity,
# so three things are arranged against that: every measurement starts from
# the same temperature, each row carries the temperature and clock it was
# taken at so drift is visible in the data instead of assumed away, and the
# run loop is round-major -- all servers once, then all servers again --
# so each server is sampled at three separate points in the thermal history
# rather than owning one contiguous stretch of it.
THERMAL_MAX_C="${THERMAL_MAX_C:-78}"
THERMAL_WAIT_S="${THERMAL_WAIT_S:-120}"
CPU_COUNT="$(nproc 2>/dev/null || echo 1)"

# name|port|command
SERVERS=(
  "forge-mt|19080|$FORGE_BIN/bench_server"
  "forge-hybrid|19084|$FORGE_BIN/bench_hybrid_server"
  "forge-uring|19087|$FORGE_BIN/bench_uring_server"
  "c-pthread|19090|env PORT=19090 $BENCH/c/run_server.sh"
  "rust-axum|19083|env PORT=19083 $BENCH/axum/bench_server/target/release/bench_server"
  "go-nethttp|19091|env PORT=19091 MODE=nethttp $BENCH/go/run_server.sh"
  "go-raw|19092|env PORT=19092 MODE=raw $BENCH/go/run_server.sh"
  "node-cluster|19093|env PORT=19093 CLUSTER=1 $BENCH/node/run_server.sh"
  "node-single|19094|env PORT=19094 CLUSTER=0 $BENCH/node/run_server.sh"
  "erlang-beam|19095|env PORT=19095 $BENCH/erlang/run_server.sh"
  "python-sync|19081|python3 $BENCH/python/server.py"
)

die() { echo "ERROR: $*" >&2; exit 1; }

[ -x "$LOADGEN" ] || die "load generator not built at $LOADGEN
  Build it with: (cd $BENCH/loadgen && go build -o loadgen .)
  This harness deliberately has no Python fallback: the Python generator
  caps near 8k rps and would report a false tie between the fast servers."
[ -f "$MONITOR" ] || die "missing $MONITOR"
[ -f "$PROBE" ] || die "missing $PROBE"

# Raise the fd ceiling: connection-per-request at high concurrency needs it
# on both sides of the loopback.
ulimit -n 1048576 2>/dev/null || ulimit -n 65536 2>/dev/null || \
  ulimit -n 4096 2>/dev/null || true

port_free() {
  ! python3 -c "
import socket,sys
try:
    socket.create_connection(('127.0.0.1', $1), 0.2).close()
except OSError:
    sys.exit(1)
" 2>/dev/null
}

# Poll until the port accepts a connection, against a real wall-clock
# deadline. The retry delay lives inside one Python process on purpose: a
# refused connection returns instantly, so a shell loop with no sleep burns
# all its iterations in milliseconds and reports "down" for any server that
# takes a moment to bind -- node-cluster forking 18 workers, for instance.
wait_up() {
  python3 - "$1" <<'PY' 2>/dev/null
import socket, sys, time
port = int(sys.argv[1])
deadline = time.monotonic() + 15.0
while time.monotonic() < deadline:
    try:
        socket.create_connection(("127.0.0.1", port), 0.2).close()
        sys.exit(0)
    except OSError:
        time.sleep(0.05)
sys.exit(1)
PY
}

# Same deadline treatment as wait_up: wait for the previous listener to
# actually go away before the next server tries to bind the same port.
wait_port_free() {
  python3 - "$1" <<'PY' 2>/dev/null && return 0
import socket, sys, time
port = int(sys.argv[1])
deadline = time.monotonic() + 15.0
while time.monotonic() < deadline:
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("127.0.0.1", port)); s.close(); sys.exit(0)
    except OSError:
        s.close(); time.sleep(0.05)
sys.exit(1)
PY
  return 1
}

kill_tree() {
  local pid="$1" port="$2"
  pkill -TERM -P "$pid" 2>/dev/null || true
  kill -TERM "$pid" 2>/dev/null || true
  # A shell loop with no delay spends its hundred iterations in
  # microseconds, so the SIGKILL below used to land before any server had
  # run a signal handler -- every one of them was measured, and torn down,
  # as if it had crashed. Wait on a real deadline instead. `kill -0` cannot
  # help here: an exited child stays visible as a zombie until wait() runs,
  # so the port going free is what "gone" actually looks like.
  python3 - "$port" <<'PY' 2>/dev/null || true
import socket, sys, time
deadline = time.monotonic() + 3.0
while time.monotonic() < deadline:
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("127.0.0.1", int(sys.argv[1]))); s.close(); sys.exit(0)
    except OSError:
        s.close(); time.sleep(0.05)
sys.exit(1)
PY
  pkill -KILL -P "$pid" 2>/dev/null || true
  kill -KILL "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

# Package temperature comes from the thermal zone rather than lm_sensors so
# the harness still runs on a box without it; an unreadable zone disables
# the gate rather than failing the suite, and the recorded nulls say so.
THERMAL_ZONE=""
for _z in /sys/class/thermal/thermal_zone*; do
  if [ "$(cat "$_z/type" 2>/dev/null)" = "x86_pkg_temp" ]; then
    THERMAL_ZONE="$_z/temp"; break
  fi
done
[ -n "$THERMAL_ZONE" ] || echo "warning: no x86_pkg_temp zone; thermal gate disabled" >&2

# Prints "<package C> <mean core MHz>", or "null null" with no sensor.
thermal_now() {
  if [ -z "$THERMAL_ZONE" ] || [ ! -r "$THERMAL_ZONE" ]; then
    echo "null null"; return
  fi
  local milli mhz
  milli="$(cat "$THERMAL_ZONE" 2>/dev/null || echo 0)"
  mhz="$(awk '/^cpu MHz/ {s += $4; n++} END {printf "%d", (n ? s / n : 0)}' \
    /proc/cpuinfo 2>/dev/null || echo 0)"
  echo "$((milli / 1000)) $mhz"
}

# Block until the package is back at or under THERMAL_MAX_C. The measured
# curve on this box falls 103 C -> 72 C in about twenty seconds and then
# floors, so the bounded wait is generous rather than tight. Hitting the
# timeout means something else is loading the machine; that is worth saying
# out loud, and the row that follows records the temperature it settled for.
thermal_settle() {
  [ -n "$THERMAL_ZONE" ] || return 0
  local t
  t="$(thermal_now | cut -d" " -f1)"
  [ "$t" = "null" ] && return 0
  [ "$t" -le "$THERMAL_MAX_C" ] && return 0
  printf '    cooling from %s C to %s C ' "$t" "$THERMAL_MAX_C" >&2
  python3 - "$THERMAL_ZONE" "$THERMAL_MAX_C" "$THERMAL_WAIT_S" <<'PY' >&2 || true
import sys, time
zone, limit, budget = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
start = time.monotonic()
c = -1
while time.monotonic() - start < budget:
    try:
        with open(zone) as f:
            c = int(f.read().strip()) // 1000
    except Exception:
        sys.exit(0)
    if c <= limit:
        sys.stderr.write("(%.0fs)\n" % (time.monotonic() - start))
        sys.exit(0)
    time.sleep(1.0)
sys.stderr.write("(gave up after %.0fs, still %d C -- machine is not idle)\n"
                 % (time.monotonic() - start, c))
PY
  return 0
}

bench_one() {
  local name="$1" port="$2" run="$3"; shift 3
  local cmd=("$@")
  local exe="${cmd[0]}"
  [ "$exe" = "env" ] && exe="$(for a in "${cmd[@]}"; do case "$a" in env|*=*) ;; *) echo "$a"; break;; esac; done)"

  if [ ! -x "$exe" ] && ! command -v "$exe" >/dev/null 2>&1; then
    printf '%-14s SKIPPED: not built (%s)\n' "$name" "$exe"
    echo "{\"server\":\"$name\",\"run\":$run,\"status\":\"skipped\",\"reason\":\"not built: $exe\"}" >>"$JSONL"
    return
  fi
  if ! port_free "$port"; then
    printf '%-14s SKIPPED: port %s already in use\n' "$name" "$port"
    echo "{\"server\":\"$name\",\"run\":$run,\"status\":\"skipped\",\"reason\":\"port $port busy\"}" >>"$JSONL"
    return
  fi

  "${cmd[@]}" >/dev/null 2>&1 &
  local pid=$!
  if ! wait_up "$port"; then
    printf '%-14s FAILED: did not accept connections on port %s\n' "$name" "$port"
    echo "{\"server\":\"$name\",\"run\":$run,\"status\":\"failed\",\"reason\":\"no listen on $port\"}" >>"$JSONL"
    kill_tree "$pid" "$port"; return
  fi

  local wire
  wire="$(python3 "$PROBE" "$port" 2>/dev/null || echo '{"ok":false,"error":"probe failed"}')"
  if [ "$(printf '%s' "$wire" | python3 -c 'import json,sys; print(json.load(sys.stdin)["ok"])')" != "True" ]; then
    printf '%-14s FAILED: wrong response, not timed -- %s\n' "$name" "$wire"
    echo "{\"server\":\"$name\",\"run\":$run,\"status\":\"failed\",\"wire\":$wire}" >>"$JSONL"
    kill_tree "$pid" "$port"; wait_port_free "$port"; return
  fi

  # Gate on temperature only once the server is up and verified, so its
  # own startup is not what pushes the package back over the limit.
  thermal_settle
  local snap t_c t_mhz
  snap="$(thermal_now)"; t_c="${snap%% *}"; t_mhz="${snap##* }"

  local mon="/tmp/forge-xlang-mon-$$-$name-$run.json"
  # Redirect the monitor's stdout: this block runs inside a
  # `... | tee $RESULTS` pipeline, and a background child that inherits
  # the pipe's write end keeps tee waiting for EOF long after the last
  # measurement is in -- the script then hangs in wait() with nothing
  # left to do and no output flushed.
  python3 "$MONITOR" "$pid" "$mon" >/dev/null 2>&1 &
  local mpid=$!
  local out
  out="$("$LOADGEN" -port "$port" -n "$REQUESTS" -c "$CONCURRENCY" -json 2>/dev/null | tail -1)"
  # The monitor writes its JSON from a SIGTERM handler, so give it a real
  # deadline to land the file before reaping. `kill -0` is no signal here:
  # an exited child stays visible as a zombie until wait() runs, so the
  # parseable file is what "done" actually looks like. The deadline is what
  # keeps one wedged monitor from hanging the entire suite.
  kill -TERM "$mpid" 2>/dev/null || true
  python3 - "$mon" <<'PY' 2>/dev/null || true
import json, sys, time
deadline = time.monotonic() + 5.0
while time.monotonic() < deadline:
  try:
      with open(sys.argv[1]) as f:
          json.load(f)
      sys.exit(0)
  except Exception:
      time.sleep(0.02)
sys.exit(1)
PY
  kill -KILL "$mpid" 2>/dev/null || true
  wait "$mpid" 2>/dev/null || true

  python3 - "$name" "$run" "$port" "$out" "$mon" "$wire" "$CPU_COUNT" \
    "$t_c" "$t_mhz" <<'PY' | tee -a "$JSONL"
import json, sys
name, run, port, out, monpath, wire, cores, t_c, t_mhz = sys.argv[1:10]
try:
  load = json.loads(out)
except Exception:
  load = {}
try:
  with open(monpath) as fh:
      mon = json.load(fh)
except Exception:
  mon = {}
w = json.loads(wire)
rps = load.get("rps") or 0.0
cpu = mon.get("cpu_seconds") or 0.0
# The loadgen names this field "elapsed_sec"; reading "elapsed_seconds"
# silently yielded 0 and turned every cores_used / rps_per_core into
# null -- the two numbers this harness exists to produce. Fall back to
# ok/rps, which is the same quantity by construction, so a future rename
# degrades into a slightly rounded figure instead of nothing at all.
el = load.get("elapsed_sec") or load.get("elapsed_seconds") or 0.0
if not el and rps and load.get("ok"):
  el = load["ok"] / rps
cores_used = round(cpu / el, 2) if el else 0.0
# The monitor samples every 100 ms, so a run that finishes inside a couple
# of ticks yields no CPU delta at all -- and then rps_per_core, the one
# number this harness exists to report, comes out null with nothing saying
# why. Carry the sample count into the row and say so on stderr, so a short
# run is legible as a short run rather than as a mysterious hole.
samples = mon.get("samples") or 0
rec = {
  "server": name, "status": "ok", "run": int(run), "port": int(port),
  "rps": round(rps, 1),
  "rps_per_core": round(rps / cores_used, 1) if cores_used else None,
  "cores_used": cores_used,
  "ok": load.get("ok"), "errors": load.get("errors"),
  "retries": load.get("retries"),
  "elapsed_seconds": el,
  "p50_ms": load.get("p50_ms"), "p90_ms": load.get("p90_ms"),
  "p99_ms": load.get("p99_ms"), "max_ms": load.get("max_ms"),
  "peak_rss_mb": mon.get("peak_rss_mb"), "peak_procs": mon.get("peak_procs"),
  "cpu_seconds": cpu,
  "response_bytes": w.get("response_bytes"),
  "extra_headers": w.get("extra_headers"),
  # Thermal state as this run started. Recorded so a reader can check that
  # the comparison was not made across a temperature gradient, instead of
  # taking that claim on faith.
  "monitor_samples": samples,
  "start_pkg_c": None if t_c == "null" else int(t_c),
  "start_mean_mhz": None if t_mhz == "null" else int(t_mhz),
}
print(json.dumps(rec))
sys.stderr.write(
  "%-14s run %s  rps=%-9.1f rps/core=%-8s cores=%-5s p50=%-6s p99=%-7s "
  "peakRSS=%-7s errs=%s retries=%s  @%sC/%sMHz\n" % (
      name, run, rps, rec["rps_per_core"], cores_used,
      rec["p50_ms"], rec["p99_ms"], rec["peak_rss_mb"],
      rec["errors"], rec["retries"], t_c, t_mhz))
if samples < 5:
  sys.stderr.write(
      "    warning: only %d monitor samples in %.3fs -- cores_used and "
      "rps_per_core are unreliable; raise REQUESTS\n" % (samples, el))
PY
  rm -f "$mon"

  kill_tree "$pid" "$port"
  wait_port_free "$port" || echo "  warning: port $port still busy after kill" >&2
}

WANTED=("$@")
: >"$JSONL"

{
  echo "Forge cross-language HTTP benchmark"
  echo "Date: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
  echo "Host: $(uname -srm), $CPU_COUNT cores, $(free -g 2>/dev/null | awk '/^Mem:/{print $2" GB RAM"}')"
  echo "Forge: $(git -C "$ROOT" rev-parse --short HEAD) ($(git -C "$ROOT" branch --show-current))"
  echo "Load: $REQUESTS requests, concurrency $CONCURRENCY, $SUITE_RUNS runs per server"
  echo
  echo "Every server returns the same 12-byte body and closes the connection."
  echo "Forge has no HTTP keep-alive (stdlib/http.c hardcodes Connection:"
  echo "close), so connection-per-request is the only shape all of these"
  echo "servers can share. Read rps_per_core, not rps: raw rps rewards a"
  echo "server for consuming more of the 18 cores."
  echo
  echo "Runs are round-major -- every server once, then every server again"
  echo "-- and each one waits for the CPU package to fall back to"
  echo "${THERMAL_MAX_C} C before it is timed. This box throttles hard under load,"
  echo "and without both of those a server's place in the list shows up in"
  echo "its numbers. Each row records the temperature and mean clock it"
  echo "started at; if those drift apart across servers, distrust the run."
  echo

  # Truncate the machine-readable log here rather than appending: each
  # record is written with `tee -a`, so without this a rerun leaves the
  # previous suite's rows in the file and the JSONL silently disagrees
  # with the .txt written beside it.
  : > "$JSONL"

  # Round-major, not server-major. Running a server's three measurements
  # back to back is the natural shape and the wrong one: it hands each
  # server its own contiguous slice of the machine's thermal history, so
  # position in the list becomes part of what is being measured. Rotating
  # through every server each round spreads that variation across all of
  # them instead of concentrating it in whoever ran last.
  for run in $(seq 1 "$SUITE_RUNS"); do
    echo "########## round $run of $SUITE_RUNS ##########"
    echo
    for entry in "${SERVERS[@]}"; do
      IFS='|' read -r name port cmd <<<"$entry"
      if [ ${#WANTED[@]} -gt 0 ]; then
        match=0
        for w in "${WANTED[@]}"; do [ "$w" = "$name" ] && match=1; done
        [ $match -eq 1 ] || continue
      fi
      echo "== $name (port $port), run $run =="
      # shellcheck disable=SC2086
      bench_one "$name" "$port" "$run" $cmd 2>&1
      echo
    done
  done
} 2>&1 | tee "$RESULTS"

echo
echo "Human-readable results: $RESULTS"
echo "Machine-readable:       $JSONL"
