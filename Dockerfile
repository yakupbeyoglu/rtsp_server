# ── Build stage ───────────────────────────────────────────────────────────────
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        pkg-config \
        git \
        ca-certificates \
        libavformat-dev \
        libavcodec-dev \
        libavutil-dev \
        libswscale-dev \
        libswresample-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/install \
    && cmake --build build --parallel \
    && cmake --install build

# ── Runtime stage ─────────────────────────────────────────────────────────────
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libavformat58 \
        libavcodec58 \
        libavutil56 \
        libswscale5 \
        libswresample3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /install/bin/rtsp-server /usr/local/bin/rtsp-server

# Default media directory inside the container
RUN mkdir -p /media
VOLUME ["/media"]

EXPOSE 554/tcp

ENTRYPOINT ["rtsp-server"]
CMD ["/media", "0.0.0.0:554"]
