# syntax=docker/dockerfile:1.7
#
# airies-ups container image.
#
# Multi-arch (amd64 + arm64) via buildx — each platform builds natively in
# its own builder container (QEMU under the hood for cross-arch). The same
# trixie base used for the Pi cross-compile, so libc/libssl/libcurl ABI
# lines up with what's installed inside the runtime layer.
#
# Frontend is xxd-embedded into the daemon binary (EMBED=1), so the runtime
# image carries no asset directory and Bun is not needed past build time.
#
# Runtime layout:
#   /opt/airies-ups/airies-upsd            daemon
#   /opt/airies-ups/airies-ups             CLI (for `docker exec` debugging)
#   /usr/share/airies-ups/udev/...         udev rules — copy to host
#   /var/lib/airies-ups/                   state volume (config.yaml, app.db)
#
# State path resolution:
#   AIRIES_UPS_CONFIG_PATH pins config.yaml inside the volume; WORKDIR puts
#   CWD there so any other CWD-relative path the daemon writes (db.path
#   defaults to "app.db") also lands in the volume.

# ---------------------------------------------------------------------------
# Builder
# ---------------------------------------------------------------------------
FROM debian:trixie AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends \
        build-essential pkg-config git curl unzip ca-certificates \
        libmodbus-dev libsqlite3-dev libcurl4-openssl-dev \
        libmicrohttpd-dev libssl-dev \
        xxd brotli && \
    rm -rf /var/lib/apt/lists/*

# Bun for the frontend bundle. Auto-detects arch — works under QEMU for arm64.
RUN curl -fsSL https://bun.sh/install | bash
ENV BUN_INSTALL=/root/.bun
ENV PATH=/root/.bun/bin:$PATH

# c-utils statically linked into the binary. Ref is overridable so CI can
# pin a specific commit per pipeline if needed; defaults to master.
#
# CUTILS_SHA is a cache-busting hint: BuildKit keys this RUN layer on the
# literal command string, so without the SHA in the command, master moving
# forward never invalidates the cached clone. CI resolves the current
# c-utils master HEAD via `git ls-remote` and passes it here, so the layer
# hash tracks upstream master.
ARG CUTILS_REPO=https://git.airies.net/vifair22/c-utils.git
ARG CUTILS_REF=master
ARG CUTILS_SHA=
RUN echo "c-utils @ ${CUTILS_REF} (sha hint: ${CUTILS_SHA:-none})" && \
    git clone --branch "${CUTILS_REF}" --depth 1 "${CUTILS_REPO}" /tmp/c-utils && \
    make -C /tmp/c-utils

WORKDIR /src
COPY . .

# Build tag composition. TARGETARCH is set automatically by buildx
# per-platform (amd64 / arm64). BUILD_VARIANT is "nightly" / "dev" / ""
# (empty on tagged-release builds). The binary's VERSION_STRING always
# carries "docker.<arch>"; the channel suffix is appended only when set.
ARG TARGETARCH
ARG BUILD_VARIANT=""

# Explicit build chain — `make all` would also run cmocka, which CI's test
# stage already covers and doesn't belong in the image build.
RUN set -eu; \
    BUILD_TAG="docker.${TARGETARCH}"; \
    if [ -n "$BUILD_VARIANT" ]; then BUILD_TAG="${BUILD_TAG}.${BUILD_VARIANT}"; fi; \
    make CUTILS_DIR=/tmp/c-utils frontend; \
    make CUTILS_DIR=/tmp/c-utils embed-frontend embed-migrations; \
    make CUTILS_DIR=/tmp/c-utils BUILD_TYPE=release EMBED=1 BUILD_TAG="$BUILD_TAG" _build

# ---------------------------------------------------------------------------
# Runtime
# ---------------------------------------------------------------------------
FROM debian:trixie-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# Trixie's t64 ABI transition renamed libssl3 → libssl3t64, libcurl4 →
# libcurl4t64, libmicrohttpd12 → libmicrohttpd12t64. libmodbus and
# libsqlite3 kept their old names.
RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends \
        libmodbus5 libsqlite3-0 libcurl4t64 libmicrohttpd12t64 libssl3t64 \
        ca-certificates && \
    rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/airies-upsd /opt/airies-ups/airies-upsd
COPY --from=builder /src/build/airies-ups  /opt/airies-ups/airies-ups
COPY 99-airies-ups-ftdi.rules \
     /usr/share/airies-ups/udev/99-airies-ups-ftdi.rules

RUN ln -s /opt/airies-ups/airies-upsd /usr/local/bin/airies-upsd && \
    ln -s /opt/airies-ups/airies-ups  /usr/local/bin/airies-ups

WORKDIR /var/lib/airies-ups
VOLUME  /var/lib/airies-ups
ENV     AIRIES_UPS_CONFIG_PATH=/var/lib/airies-ups/config.yaml

EXPOSE 8080

ENTRYPOINT ["/opt/airies-ups/airies-upsd"]
