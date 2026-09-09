#!/usr/bin/env bash
# Node.js HTTP benchmark server — same response as Forge/C/Python/Rust/Go benches.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="${ROOT}/server.js"
PORT="${PORT:-19086}"
MODE="${MODE:-raw}"
CLUSTER="${CLUSTER:-1}"

if ! command -v node >/dev/null 2>&1; then
  echo "no Node.js runtime found (node); install Node.js" >&2
  exit 1
fi

echo "Node.js benchmark server on port ${PORT} (MODE=${MODE}, CLUSTER=${CLUSTER})"
exec env PORT="${PORT}" MODE="${MODE}" CLUSTER="${CLUSTER}" node "$SRC"
