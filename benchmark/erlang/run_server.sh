#!/usr/bin/env bash
# Wrapper so the harness can launch BEAM the same way it launches every other
# server: one executable, PORT in the environment, ready banner on stdout.
set -euo pipefail
cd "$(dirname "$0")"
exec erl +P 2000000 -noshell -pa . -run bench_server main "${PORT:-19095}"
