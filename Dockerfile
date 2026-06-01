FROM ubuntu:24.04 AS builder

ARG PROXY_SERVER=""
ENV https_proxy=$PROXY_SERVER
ENV http_proxy=$PROXY_SERVER

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        xz-utils \
        file \
        git \
        cmake \
        ninja-build \
        build-essential \
        zstd \
    && rm -rf /var/lib/apt/lists/*

# install llvm
ARG LLVM_MINGW_VERSION=20260519
RUN curl -fSL "https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VERSION}/llvm-mingw-${LLVM_MINGW_VERSION}-ucrt-ubuntu-22.04-x86_64.tar.xz" -o /tmp/llvm-mingw.tar.xz \
    && mkdir -p /opt/llvm-mingw \
    && tar -xJf /tmp/llvm-mingw.tar.xz -C /opt/llvm-mingw --strip-components=1 \
    && rm /tmp/llvm-mingw.tar.xz

# Add to PATH
ENV PATH="/opt/llvm-mingw/bin:${PATH}"

# install cppwinrt
ARG WINRT_MINGW_VERSION=2.0.250303.1-2-any
RUN export MINGW_TARGET_DIR="/opt/llvm-mingw/x86_64-w64-mingw32" \
    && curl -fSL "https://mirror.msys2.org/mingw/mingw64/mingw-w64-x86_64-cppwinrt-${WINRT_MINGW_VERSION}.pkg.tar.zst" -o /tmp/mingw-cppwinrt.tar.zst \
    && tar -I zstd -xf /tmp/mingw-cppwinrt.tar.zst -C ${MINGW_TARGET_DIR}/include --strip-components=2 mingw64/include

# Copy
WORKDIR /src
COPY . .

# Deps
RUN scripts/get-deps.sh
# Build
RUN scripts/build.sh

FROM scratch AS dist
COPY --from=builder /src/dist/ /
