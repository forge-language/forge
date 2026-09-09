#!/usr/bin/env bash
# Cross-language concurrency-primitive benchmark.
#
# Measures the cost of the idiomatic cheap-concurrency unit in each runtime:
# spawn N units, each suspends once and then increments a shared counter,
# then wait for all N and assert the counter equals N.
#
# Reports, per language per N: wall-clock ms (median of REPS runs, plus
# min/max), peak RSS, and derived bytes per concurrency unit relative to the
# N=1 baseline. A hard failure (OOM, thread-creation failure, timeout) is
# recorded as a result with its actual error text, never silently dropped.
#
# See README.md for the full list of accepted asymmetries.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SRC="$HERE/src"
BUILD="$HERE/build"
RESULTS="${RESULTS:-$HERE/results/concurrency_results.txt}"
RAW="$HERE/results/raw.tsv"

# N=1 doubles as the runtime's baseline RSS for the bytes-per-unit derivation.
N_VALUES=(${N_VALUES:-1 1000 10000 100000 1000000})
REPS="${REPS:-3}"

# Per-run safety net. The cgroup cap is RSS-based (MemorySwapMax=0 turns a
# would-be swap storm into a clean OOM kill), which keeps a runaway at
# N=1000000 from taking the machine down on a host with ~5 GB free.
MEM_CAP="${MEM_CAP:-4500M}"
TIMEOUT_S="${TIMEOUT_S:-600}"

FORGE_INCLUDE="$ROOT/include"
# Shared cargo target dir: reuses the tokio already vendored for the sibling
# axum benchmark so the async-Rust build needs no network access.
CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$ROOT/benchmark/axum/bench_server/target}"
export CARGO_TARGET_DIR

mkdir -p "$BUILD" "$HERE/results"

SKIPS=()
declare -A CMD          # label -> command template ({N} substituted)
declare -A KIND         # label -> lightweight | os-thread
ORDER=()

log()  { printf '%s\n' "$*" >&2; }
skip() { log "SKIPPED: $1"; SKIPS+=("$1"); }

have() { command -v "$1" >/dev/null 2>&1; }

# --------------------------------------------------------------- toolchains ---

find_forge() {
    local c
    for c in "$ROOT/build-xlang/bin/forge" "$ROOT/build/bin/forge" "$ROOT/build-bench/bin/forge"; do
        [[ -x "$c" ]] && { printf '%s' "$c"; return 0; }
    done
    return 1
}

build_forge() {
    local fc
    if ! fc="$(find_forge)"; then
        skip "forge: no compiler at build-xlang/bin/forge, build/bin/forge or build-bench/bin/forge"
        return
    fi
    local libdir="$(dirname "$(dirname "$fc")")/lib"

    # The observable counter and the internal timer come from a small C helper
    # linked through Forge's `extern fn` FFI. Forge 0.3.0 has no module-level
    # variables, no atomics, and `on receive(...)` is parsed but never emitted
    # by codegen, so a Forge program cannot hold state shared between
    # coroutines on its own. See README "Forge counter helper".
    if ! gcc -O2 -c "$SRC/bench_counter.c" -o "$BUILD/bench_counter.o" 2>"$BUILD/forge_helper.log"; then
        skip "forge: could not build counter helper ($(tail -1 "$BUILD/forge_helper.log"))"
        return
    fi
    ar rcs "$BUILD/libbcount.a" "$BUILD/bench_counter.o" || { skip "forge: ar failed"; return; }

    # Forge's `process main` body runs to completion BEFORE the scheduler
    # drains any coroutine, and Forge has no string->int conversion, so N is a
    # compile-time constant and the program is rebuilt per N.
    local n ok=0
    for n in "${N_VALUES[@]}"; do
        sed "s/^const UNITS = .*/const UNITS = $n;/" "$SRC/bench_forge.fg" > "$BUILD/bench_forge_$n.fg"
        if "$fc" -I "$FORGE_INCLUDE" --lib-dir "$libdir" \
                 -L "$BUILD" -l bcount \
                 "$BUILD/bench_forge_$n.fg" -o "$BUILD/forge_$n" >"$BUILD/forge_$n.log" 2>&1; then
            ok=1
        else
            log "  forge: compile failed for N=$n: $(tail -1 "$BUILD/forge_$n.log")"
        fi
    done
    if (( ok )); then
        CMD[forge]="$BUILD/forge_{N}"; KIND[forge]=lightweight; ORDER+=(forge)
        log "  forge: built with $fc"
    else
        skip "forge: every compile failed (see $BUILD/forge_*.log)"
    fi
}

build_go() {
    have go || { skip "go: toolchain not found"; return; }
    # GOFLAGS=-mod=mod with no deps keeps this offline; default build is optimised.
    if GO111MODULE=off go build -o "$BUILD/bench_go" "$SRC/bench_go.go" 2>"$BUILD/go.log"; then
        CMD[go]="$BUILD/bench_go {N}"; KIND[go]=lightweight; ORDER+=(go)
        log "  go: $(go version)"
    else
        skip "go: build failed ($(tail -1 "$BUILD/go.log"))"
    fi
}

build_c() {
    have gcc || { skip "c: gcc not found"; return; }
    if gcc -O2 -pthread "$SRC/bench_c.c" -o "$BUILD/bench_c" 2>"$BUILD/c.log"; then
        CMD[c]="$BUILD/bench_c {N}"; KIND[c]=os-thread; ORDER+=(c)
        log "  c: $(gcc --version | head -1)"
    else
        skip "c: build failed ($(tail -1 "$BUILD/c.log"))"
    fi
}

build_rust_thread() {
    have cargo || { skip "rust_thread: cargo not found"; return; }
    if (cd "$HERE/rust_thread" && cargo build --release --offline >"$BUILD/rust_thread.log" 2>&1); then
        cp "$CARGO_TARGET_DIR/release/bench_rust_thread" "$BUILD/" || { skip "rust_thread: binary not produced"; return; }
        CMD[rust_thread]="$BUILD/bench_rust_thread {N}"; KIND[rust_thread]=os-thread; ORDER+=(rust_thread)
        log "  rust_thread: $(rustc --version)"
    else
        skip "rust_thread: cargo build failed ($(tail -2 "$BUILD/rust_thread.log" | tr '\n' ' '))"
    fi
}

build_rust_async() {
    have cargo || { skip "rust_async: cargo not found"; return; }
    if (cd "$HERE/rust_async" && cargo build --release --offline >"$BUILD/rust_async.log" 2>&1); then
        cp "$CARGO_TARGET_DIR/release/bench_rust_async" "$BUILD/" || { skip "rust_async: binary not produced"; return; }
        CMD[rust_async]="$BUILD/bench_rust_async {N}"; KIND[rust_async]=lightweight; ORDER+=(rust_async)
        log "  rust_async: tokio, built offline from the local registry cache"
    else
        skip "rust_async: tokio unavailable offline ($(tail -2 "$BUILD/rust_async.log" | tr '\n' ' ')) -- Rust represented by std::thread only"
    fi
}

build_java() {
    have javac && have java || { skip "java: JDK not found"; return; }
    if javac -d "$BUILD" "$SRC/BenchThread.java" 2>"$BUILD/java.log"; then
        CMD[java]="java -cp $BUILD BenchThread {N}"; KIND[java]=os-thread; ORDER+=(java)
        log "  java: $(java -version 2>&1 | head -1) -- PLATFORM threads (no virtual threads before JDK 21)"
    else
        skip "java: javac failed ($(tail -1 "$BUILD/java.log"))"
    fi
}

setup_node() {
    have node || { skip "node: not found"; return; }
    CMD[node]="node $SRC/bench_node.mjs {N}"; KIND[node]=lightweight; ORDER+=(node)
    log "  node: $(node --version)"
}

setup_python() {
    have python3 || { skip "python: python3 not found"; return; }
    CMD[python]="python3 $SRC/bench_python.py {N}"; KIND[python]=lightweight; ORDER+=(python)
    log "  python: $(python3 --version)"
}

build_runner() {
    # GNU /usr/bin/time is not installed here, so this stands in for
    # `time -v`: fork/exec + wait4, reporting ru_maxrss (the same kernel
    # counter, equal to VmHWM) and monotonic wall time. One harness for all
    # languages is what makes the RSS column comparable.
    have gcc || { log "FATAL: gcc required to build the measurement runner"; exit 1; }
    gcc -O2 "$SRC/runner.c" -o "$BUILD/runner" || { log "FATAL: runner build failed"; exit 1; }
}

# ------------------------------------------------------------------ measure ---

USE_SCOPE=0
if have systemd-run && systemd-run --user --scope -q /bin/true >/dev/null 2>&1; then
    USE_SCOPE=1
fi

# Runs one configuration once. Echoes a US-separated (0x1f) record:
#   status  wall_ms  maxrss_kb  counter  internal_ms  detail
# US rather than tab because tab is an IFS-whitespace character, which would
# collapse the empty wall/rss fields an OOM record carries.
run_once() {
    local label="$1" n="$2"
    local cmd="${CMD[$label]//\{N\}/$n}"
    # RLIMIT_AS is applied to the child only for the lightweight runtimes. The
    # OS-thread runtimes reserve ~8 MB of (lazily faulted) virtual address
    # space per thread, so an AS cap would fire on address space they never
    # touch; those are bounded by the cgroup cap and by the kernel's own
    # thread limits instead.
    local as_cap=0
    [[ "${KIND[$label]}" == lightweight ]] && as_cap=3500

    local out
    if (( USE_SCOPE )); then
        out="$(timeout $((TIMEOUT_S + 60)) systemd-run --user --scope -q \
                 -p "MemoryMax=$MEM_CAP" -p MemorySwapMax=0 \
                 "$BUILD/runner" "$as_cap" "$TIMEOUT_S" $cmd 2>&1)"
    else
        out="$(timeout $((TIMEOUT_S + 60)) "$BUILD/runner" "$as_cap" "$TIMEOUT_S" $cmd 2>&1)"
    fi

    local runner_line wall rss exit_code sig timedout counter internal
    runner_line="$(grep -m1 '^RUNNER ' <<<"$out" || true)"
    counter="$(sed -n 's/.*counter=\([0-9]*\).*/\1/p' <<<"$out" | head -1)"
    internal="$(sed -n 's/.*elapsed_ms=\([0-9.]*\).*/\1/p' <<<"$out" | head -1)"

    if [[ -z "$runner_line" ]]; then
        # The measurement harness itself never emitted a line: the whole cgroup
        # was OOM-killed (MemoryMax + MemorySwapMax=0 kills the scope, parent
        # included). That is a genuine out-of-memory result.
        printf 'OOM\x1f\x1f\x1f%s\x1f%s\x1f%s\n' "${counter:-}" "${internal:-}" \
            "cgroup OOM-kill under MemoryMax=$MEM_CAP (no harness output); $(grep -v '^RUNNER' <<<"$out" | tail -1 | tr '\t' ' ')"
        return
    fi

    wall="$(sed -n 's/.*wall_ms=\([0-9.]*\).*/\1/p' <<<"$runner_line")"
    rss="$(sed -n 's/.*maxrss_kb=\([0-9]*\).*/\1/p' <<<"$runner_line")"
    exit_code="$(sed -n 's/.*exit=\(-*[0-9]*\).*/\1/p' <<<"$runner_line")"
    sig="$(sed -n 's/.* signal=\([0-9]*\).*/\1/p' <<<"$runner_line")"
    timedout="$(sed -n 's/.*timedout=\([0-9]*\).*/\1/p' <<<"$runner_line")"

    local err_detail
    err_detail="$(grep -v '^RUNNER' <<<"$out" | grep -iE 'FAIL|error|Exception|OutOfMemory|Killed|cannot|Unable' | head -2 | tr '\n\t' '; ' )"

    if [[ "$timedout" == 1 ]]; then
        printf 'TIMEOUT\x1f%s\x1f%s\x1f%s\x1f%s\x1f%s\n' "$wall" "$rss" "${counter:-}" "${internal:-}" \
            "killed after ${TIMEOUT_S}s; ${err_detail}"
    elif [[ "$sig" != 0 ]]; then
        printf 'SIGNAL\x1f%s\x1f%s\x1f%s\x1f%s\x1f%s\n' "$wall" "$rss" "${counter:-}" "${internal:-}" \
            "killed by signal $sig$( [[ $sig == 9 ]] && printf ' (SIGKILL - OOM killer)')  ${err_detail}"
    elif [[ "$exit_code" != 0 ]]; then
        printf 'FAIL\x1f%s\x1f%s\x1f%s\x1f%s\x1f%s\n' "$wall" "$rss" "${counter:-}" "${internal:-}" \
            "exit $exit_code; ${err_detail:-no diagnostic}"
    elif [[ -z "$counter" || "$counter" != "$n" ]]; then
        printf 'WRONG_COUNT\x1f%s\x1f%s\x1f%s\x1f%s\x1f%s\n' "$wall" "$rss" "${counter:-none}" "${internal:-}" \
            "counter ${counter:-none} != N $n"
    else
        printf 'OK\x1f%s\x1f%s\x1f%s\x1f%s\x1f\n' "$wall" "$rss" "$counter" "${internal:-}"
    fi
}

median() { printf '%s\n' "$@" | sort -g | awk '{v[NR]=$1} END{ if(NR==0){print ""} else if(NR%2){print v[(NR+1)/2]} else {printf "%.3f\n",(v[NR/2]+v[NR/2+1])/2} }'; }
minv()   { printf '%s\n' "$@" | sort -g | head -1; }
maxv()   { printf '%s\n' "$@" | sort -g | tail -1; }

# ---------------------------------------------------------------------- main ---

log "=== building ==="
build_runner
build_forge
build_go
build_c
build_rust_thread
build_rust_async
build_java
setup_node
setup_python

if (( ${#ORDER[@]} == 0 )); then
    log "FATAL: no runnable configuration"
    exit 1
fi

printf 'lang\tkind\tn\trep\tstatus\twall_ms\tmaxrss_kb\tcounter\tinternal_ms\tdetail\n' > "$RAW"

declare -A MED_WALL MED_RSS MIN_WALL MAX_WALL MED_INT STATUS DETAIL

log ""
log "=== measuring (${REPS} reps per configuration) ==="
for label in "${ORDER[@]}"; do
    for n in "${N_VALUES[@]}"; do
        walls=() rsss=() ints=() worst="OK" detail=""
        for ((r = 1; r <= REPS; r++)); do
            IFS=$'\x1f' read -r st w rs cnt itn det <<<"$(run_once "$label" "$n")"
            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                "$label" "${KIND[$label]}" "$n" "$r" "$st" "$w" "$rs" "$cnt" "$itn" "$det" >> "$RAW"
            if [[ "$st" == OK ]]; then
                walls+=("$w"); rsss+=("$rs"); [[ -n "$itn" ]] && ints+=("$itn")
            else
                worst="$st"; detail="$det"
            fi
        done
        if (( ${#walls[@]} > 0 )); then
            MED_WALL[$label,$n]="$(median "${walls[@]}")"
            MIN_WALL[$label,$n]="$(minv "${walls[@]}")"
            MAX_WALL[$label,$n]="$(maxv "${walls[@]}")"
            MED_RSS[$label,$n]="$(median "${rsss[@]}")"
            (( ${#ints[@]} )) && MED_INT[$label,$n]="$(median "${ints[@]}")"
            # A configuration that failed on some reps but not others is still
            # reported as flaky rather than clean.
            STATUS[$label,$n]=$([[ ${#walls[@]} -eq $REPS ]] && echo OK || echo "PARTIAL")
        else
            STATUS[$label,$n]="$worst"
        fi
        DETAIL[$label,$n]="$detail"
        log "  $label N=$n -> ${STATUS[$label,$n]} ${MED_WALL[$label,$n]:-}ms ${MED_RSS[$label,$n]:-}KB ${detail:0:90}"
    done
done

# ---------------------------------------------------------------- reporting ---

BASE_N="${N_VALUES[0]}"

{
printf 'Cross-language concurrency-primitive benchmark\n'
printf '==============================================\n\n'
printf 'Generated : %s\n' "$(date -Is)"
printf 'Host      : %s cores, %s\n' "$(nproc)" "$(free -h | awk '/^Mem:/{print $2" total, "$7" available"}')"
printf 'Kernel    : %s\n' "$(uname -sr)"
printf 'Limits    : threads-max=%s, ulimit -u=%s\n' "$(cat /proc/sys/kernel/threads-max)" "$(ulimit -u)"
printf 'Cap       : %s\n' "$( (( USE_SCOPE )) && echo "cgroup MemoryMax=$MEM_CAP MemorySwapMax=0 per run; RLIMIT_AS=3500MB for lightweight runtimes" || echo "no cgroup cap available; RLIMIT_AS=3500MB for lightweight runtimes only" )"
printf 'Timeout   : %ss per run\n' "$TIMEOUT_S"
printf 'Reps      : %s per configuration, median reported\n' "$REPS"
printf 'RSS method: ru_maxrss via wait4() in build/runner (equivalent to GNU time -v "Maximum resident set size" / VmHWM), identical for every language\n\n'

printf 'WORK PER UNIT (identical in every language): suspend once, then increment\n'
printf 'a shared counter. Every run asserts the final counter == N; a mismatch is\n'
printf 'reported as WRONG_COUNT, never as a pass.\n\n'
printf 'Baseline for bytes/unit is each runtime at N=%s.\n\n' "$BASE_N"

printf 'Toolchains\n----------\n'
have go       && printf '  go          %s\n' "$(go version)"
have rustc    && printf '  rust        %s\n' "$(rustc --version)"
have java     && printf '  java        %s  (PLATFORM threads; JDK<21 has no virtual threads)\n' "$(java -version 2>&1 | head -1)"
have node     && printf '  node        %s\n' "$(node --version)"
have python3  && printf '  python      %s\n' "$(python3 --version)"
have gcc      && printf '  c           %s\n' "$(gcc --version | head -1)"
find_forge >/dev/null && printf '  forge       %s\n' "$("$(find_forge)" --version 2>&1 | head -1)"
printf '\n'

if (( ${#SKIPS[@]} )); then
    printf 'Skipped\n-------\n'
    for s in "${SKIPS[@]}"; do printf '  SKIPPED: %s\n' "$s"; done
    printf '\n'
fi

printf 'Results\n-------\n'
printf '%-13s %-11s %9s %11s %11s %11s %11s %10s %s\n' \
    LANG PRIMITIVE N 'WALL_MED' 'WALL_MIN' 'WALL_MAX' 'PEAK_RSS' 'B/UNIT' STATUS
printf '%-13s %-11s %9s %11s %11s %11s %11s %10s %s\n' \
    ------------- ----------- --------- ----------- ----------- ----------- ----------- ---------- ------

for label in "${ORDER[@]}"; do
    base="${MED_RSS[$label,$BASE_N]:-}"
    for n in "${N_VALUES[@]}"; do
        st="${STATUS[$label,$n]:-NOT_RUN}"
        wm="${MED_WALL[$label,$n]:-}"; wn="${MIN_WALL[$label,$n]:-}"; wx="${MAX_WALL[$label,$n]:-}"
        rs="${MED_RSS[$label,$n]:-}"
        bpu='-'
        if [[ -n "$rs" && -n "$base" && "$n" != "$BASE_N" ]]; then
            bpu="$(awk -v r="$rs" -v b="$base" -v n="$n" 'BEGIN{printf "%.0f", (r-b)*1024/n}')"
        fi
        printf '%-13s %-11s %9s %11s %11s %11s %11s %10s %s\n' \
            "$label" "${KIND[$label]}" "$n" "${wm:--}" "${wn:--}" "${wx:--}" "${rs:--}" "$bpu" "$st"
    done
    printf '\n'
done

printf 'Failures (recorded, not dropped)\n--------------------------------\n'
found=0
for label in "${ORDER[@]}"; do
    for n in "${N_VALUES[@]}"; do
        st="${STATUS[$label,$n]:-NOT_RUN}"
        [[ "$st" == OK ]] && continue
        found=1
        printf '  %s N=%s: %s\n' "$label" "$n" "$st"
        [[ -n "${DETAIL[$label,$n]:-}" ]] && printf '      %s\n' "${DETAIL[$label,$n]}"
    done
done
(( found )) || printf '  none\n'
printf '\n'

printf 'Internal (in-program) spawn+complete timing, median ms\n'
printf -- '-----------------------------------------------------\n'
printf 'Wall above is whole-process time from the external harness. This column is\n'
printf 'the runtime timing itself, started immediately before the spawn loop, so it\n'
printf 'excludes process startup. Node and Java cannot exclude JIT warmup: the timer\n'
printf 'starts before V8/HotSpot has tiered up the unit function.\n\n'
printf '%-13s' 'LANG'; for n in "${N_VALUES[@]}"; do printf '%12s' "N=$n"; done; printf '\n'
for label in "${ORDER[@]}"; do
    printf '%-13s' "$label"
    for n in "${N_VALUES[@]}"; do printf '%12s' "${MED_INT[$label,$n]:--}"; done
    printf '\n'
done
printf '\nRaw per-rep records: %s\n' "$RAW"
} > "$RESULTS"

log ""
log "=== done: $RESULTS ==="
cat "$RESULTS"
