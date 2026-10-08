FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates git cmake gcc make libc6-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DFORGE_ENABLE_GPU=OFF -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX=/opt/forge \
    && cmake --build build --parallel 2 --target forge-selfhost forge-selfhost-verify \
    && cmake --install build

FROM debian:bookworm-slim AS compiler
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 forge && useradd --uid 10001 --gid forge --create-home forge \
    && install -d -o forge -g forge /workspace
COPY --from=builder /opt/forge /opt/forge
ENV PATH="/opt/forge/bin:${PATH}" FORGE_ROOT=/opt/forge
WORKDIR /workspace
USER forge
ENTRYPOINT ["forge"]
CMD ["--help"]

# Optional build tools for Forge modules backed by external native libraries.
FROM compiler AS development
USER root
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates git cmake make pkg-config libpq-dev libmicrohttpd-dev \
    libjson-c-dev libcurl4-openssl-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
USER forge
