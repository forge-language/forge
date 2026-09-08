#!/usr/bin/env bash
# Minimal C HTTP benchmark server — same response as Forge/Python/Phoenix/Axum benches.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="${ROOT}/bench_server.c"
BIN="${ROOT}/bench_server"
PORT="${PORT:-19084}"

if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then
  echo "no C compiler found (cc/gcc); install build-essential" >&2
  exit 1
fi

CC="${CC:-cc}"
if [[ ! -x "$BIN" || "$SRC" -nt "$BIN" ]]; then
  "$CC" -O3 -pthread -o "$BIN" "$SRC"
fi

echo "C benchmark server on port ${PORT}"
exec env PORT="${PORT}" "$BIN"
