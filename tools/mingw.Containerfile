# MinGW-w64 cross-compiler for make cross-windows
FROM docker.io/library/debian:trixie-slim
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends gcc-mingw-w64-x86-64-posix make \
    && rm -rf /var/lib/apt/lists/*
