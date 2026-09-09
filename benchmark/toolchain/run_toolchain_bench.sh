#!/usr/bin/env bash
# Developer-experience / deployment toolchain benchmark for the Forge
# language, compared against C (clang), Rust, Go, Java, Node.js and
# Python. Measures compile/build time (cold + warm), artifact size,
# cold-start latency, and toolchain footprint. See README.md in this
# directory for exactly what each number means and how it was produced.
#
# Owns and only touches files under benchmark/toolchain/. Does not modify
# compiler/, runtime/, stdlib/, include/, or any other benchmark/ subdir.
set -uo pipefail

TC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TC_DIR/../.." && pwd)"
SRC_DIR="$TC_DIR/src"
RESULTS="$TC_DIR/results.txt"
SCRATCH="$TC_DIR/.scratch"
INC="$REPO_ROOT/include"

BUILD_RUNS="${BUILD_RUNS:-3}"
COLDSTART_RUNS="${COLDSTART_RUNS:-60}"
WORKERS=10000

# ---------------------------------------------------------------------
# Forge compiler discovery (per assignment: build-xlang -> build -> build-bench)
# ---------------------------------------------------------------------
FORGE_BIN=""
FORGE_BUILD_DIR=""
for cand in build-xlang build build-bench; do
    if [[ -x "$REPO_ROOT/$cand/bin/forge" ]]; then
        FORGE_BIN="$REPO_ROOT/$cand/bin/forge"
        FORGE_BUILD_DIR="$cand"
        break
    fi
done
LIB="$REPO_ROOT/${FORGE_BUILD_DIR:-build-xlang}/lib"

rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"
: > "$RESULTS"

log() { printf '%s\n' "$*" | tee -a "$RESULTS"; }
logf() { printf -- "$@" | tee -a "$RESULTS"; }

# ---------------------------------------------------------------------
# Timing helpers (pure-bash EPOCHREALTIME, no per-sample fork)
# ---------------------------------------------------------------------
epoch_us() {
    local t=$EPOCHREALTIME
    local sec=${t%.*}
    local usec=${t#*.}
    # EPOCHREALTIME always yields 6 fractional digits; guard short reads defensively
    # without going through printf %d (which misparses a leading-zero string as octal).
    while [[ ${#usec} -lt 6 ]]; do usec="${usec}0"; done
    echo $(( sec * 1000000 + 10#$usec ))
}

# median of a stream of integer microsecond samples -> prints ms (3dp)
median_ms() {
    sort -n | awk '{a[NR]=$1; n=NR} END{
        if (n==0) { print "NA"; exit }
        if (n % 2 == 1) m = a[(n+1)/2]; else m = (a[n/2] + a[n/2+1]) / 2;
        printf "%.3f", m/1000
    }'
}

# median + p99 of a stream of integer microsecond samples -> "median p99" in ms
median_p99_ms() {
    sort -n | awk '{a[NR]=$1; n=NR} END{
        if (n==0) { print "NA NA"; exit }
        if (n % 2 == 1) m = a[(n+1)/2]; else m = (a[n/2] + a[n/2+1]) / 2;
        idx = int(0.99 * n); if (idx < 1) idx = 1; if (idx > n) idx = n;
        printf "%.3f %.3f", m/1000, a[idx]/1000
    }'
}

# time_cmd RUNS -- CMD...   -> prints median ms of RUNS invocations of CMD
time_cmd_median() {
    local runs=$1; shift
    local -a samples=()
    local i s e rc=0
    for ((i = 0; i < runs; i++)); do
        s=$(epoch_us)
        "$@" >/dev/null 2>&1 || rc=$?
        e=$(epoch_us)
        samples+=( $(( e - s )) )
    done
    if [[ $rc -ne 0 ]]; then echo "FAILED"; return 1; fi
    printf '%s\n' "${samples[@]}" | median_ms
}

human_bytes() {
    local b=$1
    if command -v numfmt >/dev/null 2>&1; then
        numfmt --to=iec-i --suffix=B "$b" 2>/dev/null || echo "${b}B"
    else
        echo "${b}B"
    fi
}

file_size() { stat --format=%s "$1" 2>/dev/null || echo "NA"; }

# ---------------------------------------------------------------------
# Toolchain availability
# ---------------------------------------------------------------------
declare -A HAVE
check_tool() {
    local name=$1 cmd=$2 reason=$3
    if command -v "$cmd" >/dev/null 2>&1; then
        HAVE[$name]=1
    else
        HAVE[$name]=0
        log "SKIPPED: $name - $reason"
    fi
}

if [[ -n "$FORGE_BIN" ]]; then HAVE[forge]=1; else
    HAVE[forge]=0
    log "SKIPPED: forge - no compiler found at build-xlang/bin/forge, build/bin/forge, or build-bench/bin/forge"
fi
check_tool c clang "clang not found in PATH"
check_tool rust rustc "rustc not found in PATH"
check_tool go go "go not found in PATH"
check_tool java javac "javac not found in PATH"
check_tool node node "node not found in PATH"
check_tool python python3 "python3 not found in PATH"

HYPERFINE=0
if command -v hyperfine >/dev/null 2>&1; then
    HYPERFINE=1
fi

log "Forge toolchain benchmark"
log "Date: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
log "Host: $(uname -a)"
log "CPU cores: $(nproc 2>/dev/null || echo unknown)"
log "Build runs per measurement: $BUILD_RUNS (median reported)"
log "Cold-start iterations: $COLDSTART_RUNS"
if [[ $HYPERFINE -eq 1 ]]; then
    log "Cold-start timer: hyperfine"
else
    log "Cold-start timer: bash EPOCHREALTIME loop fallback (hyperfine not installed) -- median and p99 over $COLDSTART_RUNS runs"
fi
[[ -n "$FORGE_BIN" ]] && log "Forge binary: $FORGE_BIN (build dir: $FORGE_BUILD_DIR)"
log "Versions:"
[[ ${HAVE[c]:-0} -eq 1 ]]      && log "  clang:  $(clang --version | head -1)"
[[ ${HAVE[rust]:-0} -eq 1 ]]   && log "  rustc:  $(rustc --version)"
[[ ${HAVE[go]:-0} -eq 1 ]]     && log "  go:     $(go version)"
[[ ${HAVE[java]:-0} -eq 1 ]]   && log "  javac:  $(javac --version 2>&1)"
[[ ${HAVE[node]:-0} -eq 1 ]]   && log "  node:   $(node --version)"
[[ ${HAVE[python]:-0} -eq 1 ]] && log "  python: $(python3 --version)"
log "Note on -j1 / single-threaded builds (per task instructions, 18 cores available):"
log "  go:    go build -p 1 (caps parallel build actions at 1)"
log "  rust:  rustc -C codegen-units=1 (forces serial single-unit codegen; rustc has no"
log "         stable frontend -j flag, this is the closest real knob)"
log "  clang/forge/javac: single translation unit / single file, no parallelism to disable"
log ""

# =======================================================================
# SECTION 1: COMPILE / BUILD TIME
# =======================================================================
log "=== SECTION 1: COMPILE / BUILD TIME (median of $BUILD_RUNS runs, milliseconds) ==="
log ""

# --- generic cold/warm driver -----------------------------------------
# build_cold RUNS WORKDIR_PREFIX -- CMD building from a pristine copy each time
run_cold() {
    local runs=$1 workdir_prefix=$2; shift 2
    local -a samples=()
    local i s e rc=0 d
    for ((i = 0; i < runs; i++)); do
        d="$(mktemp -d "${SCRATCH}/${workdir_prefix}.cold.XXXXXX")"
        s=$(epoch_us)
        ( cd "$d" && "$@" ) >/dev/null 2>&1 || rc=$?
        e=$(epoch_us)
        samples+=( $(( e - s )) )
        rm -rf "$d"
    done
    if [[ $rc -ne 0 ]]; then echo "FAILED"; return 1; fi
    printf '%s\n' "${samples[@]}" | median_ms
}

print_row() {
    printf '  %-8s %-14s %-6s %10s ms\n' "$1" "$2" "$3" "$4" | tee -a "$RESULTS"
}

# ---------------- Forge ----------------
if [[ ${HAVE[forge]:-0} -eq 1 ]]; then
    forge_build() { "$FORGE_BIN" -I "$INC" --lib-dir "$LIB" "$1" -o "$2"; }

    for prog in hello concurrency; do
        SRC="$SRC_DIR/forge/$prog.fg"

        # cold: full native build (frontend + clang compile + link), fresh dir each run
        t=$(run_cold "$BUILD_RUNS" "forge-$prog" bash -c "\"$FORGE_BIN\" -I \"$INC\" --lib-dir \"$LIB\" \"$SRC\" -o out")
        print_row forge "$prog" cold "$t"

        # warm: persistent dir, real source edit before each rebuild
        wd="$SCRATCH/forge-$prog.warm"; mkdir -p "$wd"
        cp "$SRC" "$wd/work.fg"
        forge_build "$wd/work.fg" "$wd/out" >/dev/null 2>&1  # prime once
        samples=()
        for ((i = 0; i < BUILD_RUNS; i++)); do
            echo "// warm-edit-$i" >> "$wd/work.fg"
            s=$(epoch_us)
            forge_build "$wd/work.fg" "$wd/out" >/dev/null 2>&1
            e=$(epoch_us)
            samples+=( $(( e - s )) )
        done
        t=$(printf '%s\n' "${samples[@]}" | median_ms)
        print_row forge "$prog" warm "$t"

        # --- the .fg -> C vs clang split (cold, this is the headline number) ---
        d="$SCRATCH/forge-$prog.split"; mkdir -p "$d"
        samples_emit=(); samples_compile=(); samples_link=(); samples_total=()
        for ((i = 0; i < BUILD_RUNS; i++)); do
            s=$(epoch_us)
            "$FORGE_BIN" -I "$INC" --lib-dir "$LIB" "$SRC" -o "$d/native.$i" >/dev/null 2>&1
            e=$(epoch_us); samples_total+=( $(( e - s )) )

            s=$(epoch_us)
            "$FORGE_BIN" -I "$INC" --lib-dir "$LIB" "$SRC" -o "$d/out.$i.c" --emit-c >/dev/null 2>&1
            e=$(epoch_us); samples_emit+=( $(( e - s )) )

            s=$(epoch_us)
            clang -std=c11 -O3 -I "$INC" -c "$d/out.$i.c" -o "$d/out.$i.o" >/dev/null 2>&1
            e=$(epoch_us); samples_compile+=( $(( e - s )) )

            s=$(epoch_us)
            clang "$d/out.$i.o" -o "$d/out.$i.bin" -L "$LIB" -lforge_std -lforge_runtime -lm >/dev/null 2>&1
            e=$(epoch_us); samples_link+=( $(( e - s )) )
        done
        m_total=$(printf '%s\n' "${samples_total[@]}" | median_ms)
        m_emit=$(printf '%s\n' "${samples_emit[@]}" | median_ms)
        m_compile=$(printf '%s\n' "${samples_compile[@]}" | median_ms)
        m_link=$(printf '%s\n' "${samples_link[@]}" | median_ms)
        log "    -> $prog split: forge-frontend(.fg->C)=${m_emit}ms  clang-compile(.c->.o)=${m_compile}ms  clang-link=${m_link}ms  (sum=$(awk -v a=$m_emit -v b=$m_compile -v c=$m_link 'BEGIN{printf "%.3f", a+b+c}')ms vs directly-measured forge-native-build=${m_total}ms)"
    done
    log ""
else
    print_row forge hello n/a "SKIPPED"
    print_row forge concurrency n/a "SKIPPED"
fi

# ---------------- C (clang) ----------------
if [[ ${HAVE[c]:-0} -eq 1 ]]; then
    for prog in hello concurrency; do
        SRC="$SRC_DIR/c/$prog.c"
        t=$(run_cold "$BUILD_RUNS" "c-$prog" bash -c "clang -O3 -std=c11 -pthread \"$SRC\" -o out")
        print_row c "$prog" cold "$t"

        wd="$SCRATCH/c-$prog.warm"; mkdir -p "$wd"
        cp "$SRC" "$wd/work.c"
        clang -O3 -std=c11 -pthread "$wd/work.c" -o "$wd/out" >/dev/null 2>&1
        samples=()
        for ((i = 0; i < BUILD_RUNS; i++)); do
            echo "// warm-edit-$i" >> "$wd/work.c"
            s=$(epoch_us)
            clang -O3 -std=c11 -pthread "$wd/work.c" -o "$wd/out" >/dev/null 2>&1
            e=$(epoch_us)
            samples+=( $(( e - s )) )
        done
        t=$(printf '%s\n' "${samples[@]}" | median_ms)
        print_row c "$prog" warm "$t"
    done
    log ""
else
    print_row c hello n/a "SKIPPED"
    print_row c concurrency n/a "SKIPPED"
fi

# ---------------- Rust ----------------
if [[ ${HAVE[rust]:-0} -eq 1 ]]; then
    for prog in hello concurrency; do
        SRC="$SRC_DIR/rust/$prog.rs"
        t=$(run_cold "$BUILD_RUNS" "rust-$prog" bash -c "rustc -C opt-level=3 -C codegen-units=1 \"$SRC\" -o out")
        print_row rust "$prog" cold "$t"

        wd="$SCRATCH/rust-$prog.warm"; mkdir -p "$wd"
        cp "$SRC" "$wd/work.rs"
        rustc -C opt-level=3 -C codegen-units=1 "$wd/work.rs" -o "$wd/out" >/dev/null 2>&1
        samples=()
        for ((i = 0; i < BUILD_RUNS; i++)); do
            echo "// warm-edit-$i" >> "$wd/work.rs"
            s=$(epoch_us)
            rustc -C opt-level=3 -C codegen-units=1 "$wd/work.rs" -o "$wd/out" >/dev/null 2>&1
            e=$(epoch_us)
            samples+=( $(( e - s )) )
        done
        t=$(printf '%s\n' "${samples[@]}" | median_ms)
        print_row rust "$prog" warm "$t"
    done
    log ""
else
    print_row rust hello n/a "SKIPPED"
    print_row rust concurrency n/a "SKIPPED"
fi

# ---------------- Go ----------------
if [[ ${HAVE[go]:-0} -eq 1 ]]; then
    for prog in hello concurrency; do
        SRC="$SRC_DIR/go/$prog.go"
        # cold: fresh GOCACHE per run as well as fresh workdir (no cache at all)
        samples=()
        rc=0
        for ((i = 0; i < BUILD_RUNS; i++)); do
            d="$(mktemp -d "${SCRATCH}/go-$prog.cold.XXXXXX")"
            gc="$(mktemp -d "${SCRATCH}/go-$prog.gocache.XXXXXX")"
            s=$(epoch_us)
            ( cd "$d" && GOCACHE="$gc" go build -p 1 -o out "$SRC" ) >/dev/null 2>&1 || rc=$?
            e=$(epoch_us)
            samples+=( $(( e - s )) )
            rm -rf "$d" "$gc"
        done
        if [[ $rc -ne 0 ]]; then t="FAILED"; else t=$(printf '%s\n' "${samples[@]}" | median_ms); fi
        print_row go "$prog" cold "$t"

        # warm: persistent GOCACHE + persistent workdir, real edit before each rebuild
        wd="$SCRATCH/go-$prog.warm"; mkdir -p "$wd"; gc="$SCRATCH/go-$prog.warm.gocache"; mkdir -p "$gc"
        cp "$SRC" "$wd/work.go"
        ( cd "$wd" && GOCACHE="$gc" go build -p 1 -o out work.go ) >/dev/null 2>&1
        samples=()
        for ((i = 0; i < BUILD_RUNS; i++)); do
            echo "// warm-edit-$i" >> "$wd/work.go"
            s=$(epoch_us)
            ( cd "$wd" && GOCACHE="$gc" go build -p 1 -o out work.go ) >/dev/null 2>&1
            e=$(epoch_us)
            samples+=( $(( e - s )) )
        done
        t=$(printf '%s\n' "${samples[@]}" | median_ms)
        print_row go "$prog" warm "$t"
    done
    log ""
else
    print_row go hello n/a "SKIPPED"
    print_row go concurrency n/a "SKIPPED"
fi

# ---------------- Java ----------------
if [[ ${HAVE[java]:-0} -eq 1 ]]; then
    declare -A JAVA_CLASS=( [hello]=Hello [concurrency]=Concurrency )
    for prog in hello concurrency; do
        cls="${JAVA_CLASS[$prog]}"
        SRC="$SRC_DIR/java/$cls.java"
        t=$(run_cold "$BUILD_RUNS" "java-$prog" bash -c "javac -d . \"$SRC\"")
        print_row java "$prog" cold "$t"

        wd="$SCRATCH/java-$prog.warm"; mkdir -p "$wd"
        cp "$SRC" "$wd/$cls.java"
        javac -d "$wd" "$wd/$cls.java" >/dev/null 2>&1
        samples=()
        for ((i = 0; i < BUILD_RUNS; i++)); do
            echo "// warm-edit-$i" >> "$wd/$cls.java"
            s=$(epoch_us)
            javac -d "$wd" "$wd/$cls.java" >/dev/null 2>&1
            e=$(epoch_us)
            samples+=( $(( e - s )) )
        done
        t=$(printf '%s\n' "${samples[@]}" | median_ms)
        print_row java "$prog" warm "$t"
    done
    log ""
else
    print_row java hello n/a "SKIPPED"
    print_row java concurrency n/a "SKIPPED"
fi

# ---------------- Node / Python: interpreted, no compile step ----------------
for lang in node python; do
    for prog in hello concurrency; do
        print_row "$lang" "$prog" cold "n/a (interpreted)"
        print_row "$lang" "$prog" warm "n/a (interpreted)"
    done
done
log ""

# =======================================================================
# SECTION 2: ARTIFACT SIZE
# =======================================================================
log "=== SECTION 2: ARTIFACT SIZE ==="
log ""

report_binary_size() {
    local lang=$1 out=$2
    if [[ ! -f "$out" ]]; then
        log "  $lang: build artifact missing, skipping size report"
        return
    fi
    local unstripped stripped
    unstripped=$(file_size "$out")
    cp "$out" "$out.stripped"
    strip "$out.stripped" 2>/dev/null
    stripped=$(file_size "$out.stripped")
    log "  $lang: unstripped=$(human_bytes "$unstripped") ($unstripped B)  stripped=$(human_bytes "$stripped") ($stripped B)"
    log "    ldd: $(ldd "$out" 2>&1 | tr '\n' ' ' | sed 's/  */ /g')"
}

ARTIFACT_DIR="$SCRATCH/artifacts"; mkdir -p "$ARTIFACT_DIR"

if [[ ${HAVE[forge]:-0} -eq 1 ]]; then
    "$FORGE_BIN" -I "$INC" --lib-dir "$LIB" "$SRC_DIR/forge/hello.fg" -o "$ARTIFACT_DIR/forge_hello" >/dev/null 2>&1
    report_binary_size "forge (hello)" "$ARTIFACT_DIR/forge_hello"
fi
if [[ ${HAVE[c]:-0} -eq 1 ]]; then
    clang -O3 -std=c11 -pthread "$SRC_DIR/c/hello.c" -o "$ARTIFACT_DIR/c_hello" >/dev/null 2>&1
    report_binary_size "c (hello)" "$ARTIFACT_DIR/c_hello"
fi
if [[ ${HAVE[rust]:-0} -eq 1 ]]; then
    rustc -C opt-level=3 -C codegen-units=1 "$SRC_DIR/rust/hello.rs" -o "$ARTIFACT_DIR/rust_hello" >/dev/null 2>&1
    report_binary_size "rust (hello)" "$ARTIFACT_DIR/rust_hello"
fi
if [[ ${HAVE[go]:-0} -eq 1 ]]; then
    go build -o "$ARTIFACT_DIR/go_hello" "$SRC_DIR/go/hello.go" >/dev/null 2>&1
    report_binary_size "go (hello)" "$ARTIFACT_DIR/go_hello"
fi
if [[ ${HAVE[java]:-0} -eq 1 ]]; then
    javac -d "$ARTIFACT_DIR" "$SRC_DIR/java/Hello.java" >/dev/null 2>&1
    cls_size=$(file_size "$ARTIFACT_DIR/Hello.class")
    if command -v jar >/dev/null 2>&1; then
        ( cd "$ARTIFACT_DIR" && jar cfe hello.jar Hello Hello.class ) >/dev/null 2>&1
        jar_size=$(file_size "$ARTIFACT_DIR/hello.jar")
        log "  java (hello): Hello.class=$(human_bytes "$cls_size") ($cls_size B), hello.jar=$(human_bytes "$jar_size") ($jar_size B)"
    else
        log "  java (hello): Hello.class=$(human_bytes "$cls_size") ($cls_size B)"
    fi
    log "    runtime dependency: requires a JVM to run (this machine's JDK 17 install is ~421 MiB, see SECTION 4); the .class file is not a standalone deployable artifact"
fi
if [[ ${HAVE[node]:-0} -eq 1 ]]; then
    src_size=$(file_size "$SRC_DIR/node/hello.js")
    log "  node (hello): hello.js source=$(human_bytes "$src_size") ($src_size B)"
    log "    runtime dependency: requires the Node.js runtime to execute (this machine's node binary is ~118 MiB, see SECTION 4); not a standalone deployable artifact"
fi
if [[ ${HAVE[python]:-0} -eq 1 ]]; then
    src_size=$(file_size "$SRC_DIR/python/hello.py")
    log "  python (hello): hello.py source=$(human_bytes "$src_size") ($src_size B)"
    log "    runtime dependency: requires a Python 3 interpreter to execute (this machine's python3 install is ~73 MiB, see SECTION 4); not a standalone deployable artifact"
fi
log ""

# =======================================================================
# SECTION 3: COLD START (hello world, time to first byte of output)
# =======================================================================
log "=== SECTION 3: COLD START (hello world, ms; median / p99 over $COLDSTART_RUNS runs) ==="
log ""

measure_coldstart() {
    local label=$1; shift
    local -a samples=()
    local i s e
    for ((i = 0; i < COLDSTART_RUNS; i++)); do
        s=$(epoch_us)
        "$@" >/dev/null 2>&1
        e=$(epoch_us)
        samples+=( $(( e - s )) )
    done
    local result
    result=$(printf '%s\n' "${samples[@]}" | median_p99_ms)
    log "  $label: median=$(echo "$result" | awk '{print $1}')ms p99=$(echo "$result" | awk '{print $2}')ms"
}

[[ -f "$ARTIFACT_DIR/forge_hello" ]] && measure_coldstart "forge " "$ARTIFACT_DIR/forge_hello"
[[ -f "$ARTIFACT_DIR/c_hello" ]]     && measure_coldstart "c     " "$ARTIFACT_DIR/c_hello"
[[ -f "$ARTIFACT_DIR/rust_hello" ]]  && measure_coldstart "rust  " "$ARTIFACT_DIR/rust_hello"
[[ -f "$ARTIFACT_DIR/go_hello" ]]    && measure_coldstart "go    " "$ARTIFACT_DIR/go_hello"
if [[ ${HAVE[java]:-0} -eq 1 && -f "$ARTIFACT_DIR/Hello.class" ]]; then
    measure_coldstart "java  " java -cp "$ARTIFACT_DIR" Hello
fi
if [[ ${HAVE[node]:-0} -eq 1 ]]; then
    measure_coldstart "node  " node "$SRC_DIR/node/hello.js"
fi
if [[ ${HAVE[python]:-0} -eq 1 ]]; then
    measure_coldstart "python" python3 "$SRC_DIR/python/hello.py"
fi
log ""

# =======================================================================
# SECTION 4: TOOLCHAIN FOOTPRINT (installed size, best-effort)
# =======================================================================
log "=== SECTION 4: TOOLCHAIN FOOTPRINT (installed size, best-effort) ==="
log ""

pacman_size() {
    local pkg=$1
    pacman -Qi "$pkg" 2>/dev/null | awk -F': ' '/^Installed Size/{print $2}'
}

if [[ -n "$FORGE_BIN" ]]; then
    fs=$(du -sh "$FORGE_BIN" 2>/dev/null | awk '{print $1}')
    ls_=$(du -sh "$LIB" 2>/dev/null | awk '{print $1}')
    log "  forge: compiler binary=${fs:-unknown}, libforge_*.a dir=${ls_:-unknown}"
    log "    Forge has no toolchain of its own for the C stage -- it shells out to the"
    log "    system C compiler (clang, by default here), whose footprint is the 'c' row below."
fi
if [[ ${HAVE[c]:-0} -eq 1 ]]; then
    v=$(pacman_size clang); log "  c (clang via pacman): ${v:-unknown}"
fi
if [[ ${HAVE[rust]:-0} -eq 1 ]]; then
    if [[ -d "$HOME/.rustup/toolchains" ]]; then
        v=$(du -sh "$HOME/.rustup/toolchains"/*/ 2>/dev/null | awk '{print $1}' | head -1)
        cb=$(du -sh "$HOME/.cargo/bin" 2>/dev/null | awk '{print $1}')
        log "  rust (rustup toolchain dir): ${v:-unknown} + cargo/rustup bin ${cb:-unknown}"
        log "    (excludes ~/.cargo/registry -- that's downloaded crate cache from unrelated"
        log "    builds, not part of the rust toolchain itself)"
    else
        v=$(pacman_size rust); log "  rust: ${v:-unknown}"
    fi
fi
if [[ ${HAVE[go]:-0} -eq 1 ]]; then
    v=$(pacman_size go); log "  go (pacman): ${v:-unknown}"
fi
if [[ ${HAVE[java]:-0} -eq 1 ]]; then
    v=$(pacman_size jdk17-openjdk); log "  java (jdk17-openjdk via pacman): ${v:-unknown}"
fi
if [[ ${HAVE[node]:-0} -eq 1 ]]; then
    node_bin="$(command -v node)"
    v=$(du -sh "$node_bin" 2>/dev/null | awk '{print $1}')
    log "  node (runtime binary only): ${v:-unknown}"
    log "    (this machine's nvm node dir also contains ~1.6GB of unrelated globally-"
    log "    installed npm packages -- excluded, since those aren't the node toolchain)"
fi
if [[ ${HAVE[python]:-0} -eq 1 ]]; then
    v=$(pacman_size python); log "  python (pacman): ${v:-unknown}"
fi
log ""

log "Done. Full results written to $RESULTS"
rm -rf "$SCRATCH"
