# Forge compiler containers

Build a native Linux Forge SDK from this checkout. CMake downloads the runtime and
standard library at the immutable commits in `cmake/Dependencies.cmake`. The build
also verifies the self-hosting compiler fixed point. No package registry account is
required for a local image.

```sh
docker build -f Containerfile --target compiler -t forge-lang:latest .
docker run --rm forge-lang:latest --version
docker run --rm forge-lang:latest --help
```

The image contains `forge`, `forge-fg`, runtime and standard-library headers and
libraries, and GCC for compiling generated C. It uses Debian Bookworm and runs as
user `forge` (UID 10001) in `/workspace`. Pass your host UID/GID when mounting a
source directory so generated outputs belong to you.

```sh
mkdir -p docker-example
printf 'native main { println("Hello, Forge!"); return 0; }\n' > docker-example/main.fg

docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD/docker-example:/workspace" forge-lang:latest main.fg -o hello

docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD/docker-example:/workspace" --entrypoint /workspace/hello forge-lang:latest

docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD/docker-example:/workspace" forge-lang:latest main.fg --emit-js -o main.js
```

Run the compiled executable inside the container: binaries target the image's
architecture and Linux C library. `--emit-js` writes JavaScript; Node.js and browser
execution are outside this image. Forge processes/coroutines remain native-only.

For modules that require Git, CMake, pkg-config, PostgreSQL or Web native libraries:

```sh
docker build -f Containerfile --target development -t forge-lang:dev .
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD:/workspace" --entrypoint /bin/sh forge-lang:dev
```

The development image provides native build dependencies. It does not include the
separate `forge pkg` package-manager distribution. Use the SDK installer documented
at https://forge-lang.org/docs if you need its package-management commands.

The original `Dockerfile` remains available. `Containerfile` adds explicit
`compiler` and `development` targets and an unprivileged default user. The
`container.yml` workflow builds the compiler target and validates native output and
JavaScript generation on changes; it does not publish a registry image.
