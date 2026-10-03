#!/bin/sh
set -eu
if [ "$#" != 4 ]; then
  echo "Usage: sh portfolio_improve_build.sh SNAPSHOT_AFTER RUNTIME_IMAGE CANDIDATE_WEB OUTPUT_DIRECTORY" >&2
  exit 2
fi
SNAPSHOT=$(CDPATH= cd -- "$1" && pwd)
IMAGE=$2
WEB=$(CDPATH= cd -- "$3" && pwd)
mkdir -p "$4"
OUTPUT=$(CDPATH= cd -- "$4" && pwd)
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -e "$OUTPUT/portfolio-baseline-profile" ] || [ -e "$OUTPUT/portfolio-fixed" ]; then
  echo 'Refusing existing build artifacts' >&2
  exit 1
fi
docker run --rm --entrypoint sh \
  -v "$SNAPSHOT:/src:ro" -v "$WEB:/candidate:ro" -v "$OUTPUT:/out" \
  -v "$HERE/portfolio_profile.c:/profile.c:ro" "$IMAGE" -ec '
  apk add --no-cache build-base cmake pkgconf libpq-dev json-c-dev libmicrohttpd-dev curl-dev openssl-dev
  BUILD_DIR=/tmp/build sh /src/backend-forge/scripts/build.sh
  cmake -S /candidate -B /tmp/web-fixed -DCMAKE_BUILD_TYPE=Release
  cmake --build /tmp/web-fixed -j 4
  ctest --test-dir /tmp/web-fixed --output-on-failure
  link() {
    cc -std=gnu11 -O2 -Wall -Wextra -Werror=implicit-function-declaration \
      -Werror=incompatible-pointer-types -Wno-unused-function \
      -I /src/backend-forge/toolchain/include -I /candidate/include \
      /tmp/build/server.c ${PROFILE_SOURCE:-} /tmp/build/postgres/libforge_postgres.a \
      "$1" /tmp/build/toolchain/lib/libforge_runtime.a /tmp/build/toolchain/lib/libforge_std.a \
      $(pkg-config --cflags --libs libpq libmicrohttpd json-c libcurl openssl) \
      -lpthread -lm ${PROFILE_FLAGS:-} -o "$2"
  }
  link /tmp/web-fixed/libforge_web.a /out/portfolio-fixed
  PROFILE_SOURCE=/profile.c
  PROFILE_FLAGS="-Wl,--wrap=fw_run -Wl,--wrap=fpg_acquire -Wl,--wrap=fpg_query_prepared -Wl,--wrap=fpg_query"
  link /tmp/build/web/libforge_web.a /out/portfolio-baseline-profile
  link /tmp/web-fixed/libforge_web.a /out/portfolio-fixed-profile
  cp /tmp/web-fixed/http_server_test /tmp/web-fixed/http_client_test /out/
  cp /tmp/build/server.c /out/server.c
  cp /candidate/src/bridge.c /out/candidate-web.c
  cc --version > /out/compiler.txt
  apk info -v > /out/build-packages.txt
  '
