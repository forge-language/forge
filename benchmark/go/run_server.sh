#!/usr/bin/env bash
# Go HTTP benchmark server — same response as Forge/C/Python/Rust benches.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="${ROOT}/server.go"
BIN="${ROOT}/bench_server_go"
PORT="${PORT:-19085}"
MODE="${MODE:-nethttp}"

if ! command -v go >/dev/null 2>&1; then
  echo "no Go toolchain found (go); install Go" >&2
  exit 1
fi

if [[ ! -x "$BIN" || "$SRC" -nt "$BIN" ]]; then
  (cd "$ROOT" && GOFLAGS=-mod=mod GOPROXY=off go build -o "$BIN" server.go)
fi

echo "Go benchmark server on port ${PORT} (MODE=${MODE})"
exec env PORT="${PORT}" MODE="${MODE}" "$BIN"
