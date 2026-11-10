# ==========================================
# STAGE 1: Builder
# ==========================================
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN BUILDER_TOOLS="build-essential cmake ninja-build git curl zip unzip tar \
    pkg-config python3 bison flex autoconf autoconf-archive automake libtool" && \
    apt-get update && \
    (apt-get install -y -o Acquire::Retries=5 ${BUILDER_TOOLS} || \
     (apt-get update && apt-get install -y -o Acquire::Retries=5 ${BUILDER_TOOLS})) && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /opt

RUN git clone https://github.com/microsoft/vcpkg.git && \
    git -C vcpkg checkout 9a5dfef5d1a0438a7d24856a85bd60f309d355e9 && \
    ./vcpkg/bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg

WORKDIR /app

COPY vcpkg.json ./

# Persistent BuildKit caches
RUN --mount=type=cache,target=/opt/vcpkg/downloads,sharing=locked \
    --mount=type=cache,target=/var/cache/vcpkg-archives,id=vcpkg-archives,sharing=locked \
    mkdir -p /opt/vcpkg/downloads && \
    curl -fsSL --retry 10 --retry-all-errors --retry-delay 3 \
        -o /opt/vcpkg/downloads/cmake-4.4.4-linux-x86_64.tar.gz \
        https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-linux-x86_64.tar.gz && \
    X_VCPKG_BINARY_SOURCES="clear;files,/var/cache/vcpkg-archives,readwrite" \
    ${VCPKG_ROOT}/vcpkg install --triplet x64-linux

COPY . .

RUN rm -rf build && cmake -B build -S . \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake

RUN cmake --build build --config Release

# ==========================================
# STAGE 2: Runtime
# ==========================================
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN RUNTIME_TOOLS="ca-certificates libstdc++6" && \
    apt-get update && \
    (apt-get install -y -o Acquire::Retries=5 ${RUNTIME_TOOLS} || \
     (apt-get update && apt-get install -y -o Acquire::Retries=5 ${RUNTIME_TOOLS})) && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

RUN mkdir -p /var/log/runner /tmp/runner /opt/runner/ssl

COPY --from=builder /app/build/runner /opt/runner/runner
COPY --from=builder /app/runner.cfg /etc/runner/runner.cfg

EXPOSE 8080 8081 9090 9100

WORKDIR /opt/runner
CMD ["./runner"]