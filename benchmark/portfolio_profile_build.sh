#!/bin/sh
# Build plain and instrumented controls without changing application sources/images.
set -eu
if [ "$#" != 3 ]; then
  echo "Usage: sh portfolio_profile_build.sh SNAPSHOT_AFTER RUNTIME_IMAGE OUTPUT_DIRECTORY" >&2
  exit 2
fi
SNAPSHOT=$(CDPATH= cd -- "$1" && pwd)
IMAGE=$2
mkdir -p "$3"
OUTPUT=$(CDPATH= cd -- "$3" && pwd)
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
for name in portfolio-plain portfolio-profile server.c compiler.txt build-packages.txt; do
  if [ -e "$OUTPUT/$name" ]; then
    echo "Refusing to overwrite $OUTPUT/$name" >&2
    exit 1
  fi
done
docker run --rm --entrypoint sh \
  -v "$SNAPSHOT:/src:ro" -v "$OUTPUT:/out" \
  -v "$HERE/portfolio_profile.c:/profile.c:ro" "$IMAGE" -ec '
  apk add --no-cache build-base cmake pkgconf libpq-dev json-c-dev libmicrohttpd-dev curl-dev openssl-dev
  BUILD_DIR=/tmp/build sh /src/backend-forge/scripts/build.sh
  cp /tmp/build/portfolio-api /out/portfolio-plain
  cc -std=gnu11 -O2 -Wall -Wextra -Werror=implicit-function-declaration \
    -Werror=incompatible-pointer-types -Wno-unused-function \
    -I /src/backend-forge/toolchain/include -I /src/backend-forge/vendor/forge-web/include \
    /tmp/build/server.c /profile.c /tmp/build/postgres/libforge_postgres.a \
    /tmp/build/web/libforge_web.a /tmp/build/toolchain/lib/libforge_runtime.a \
    /tmp/build/toolchain/lib/libforge_std.a \
    $(pkg-config --cflags --libs libpq libmicrohttpd json-c libcurl openssl) \
    -lpthread -lm -Wl,--wrap=fw_run -Wl,--wrap=fpg_acquire \
    -Wl,--wrap=fpg_query_prepared -Wl,--wrap=fpg_query -o /out/portfolio-profile
  cp /tmp/build/server.c /out/server.c
  cc --version > /out/compiler.txt
  apk info -v > /out/build-packages.txt
  '
