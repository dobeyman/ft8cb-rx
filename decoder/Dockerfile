FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    gcc \
    g++ \
    make \
    libfftw3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Copy ft8_lib source
COPY ft8_lib/ ./ft8_lib/

# Copy our decoder
COPY ft8cb_decode.cpp ./

# Build ft8_lib (libft8.a) + fft objects
RUN cd ft8_lib && \
    make libft8.a && \
    mkdir -p .build/fft && \
    for f in fft/*.c; do \
        cc -O3 -DHAVE_STPCPY -I. -o .build/${f%.c}.o -c $f; \
    done

# Build ft8cb_decode (link ft8 lib + fft kiss objects)
RUN g++ -O2 -std=c++17 \
    ft8cb_decode.cpp \
    ft8_lib/fft/kiss_fft.c \
    ft8_lib/fft/kiss_fftr.c \
    -I ft8_lib \
    -L ft8_lib \
    -lft8 \
    -lm \
    -o ft8cb_decode

# ─────────────────────────────────────────────────────────
FROM debian:bookworm-slim

# Runtime dependencies
RUN apt-get update && apt-get install -y \
    libfftw3-3 \
    rtl-sdr \
    python3 \
    python3-pip \
    && pip3 install kiwiclient --break-system-packages 2>/dev/null || true \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/ft8cb_decode /usr/local/bin/ft8cb_decode
COPY audio_bridge.py /usr/local/bin/audio_bridge.py
RUN chmod +x /usr/local/bin/audio_bridge.py

# Create output directory
RUN mkdir -p /tmp/ft8cb

# Environment defaults
ENV SOURCE=rtlsdr \
    FREQUENCY=27265000 \
    RTLSDR_GAIN=40 \
    RTLSDR_DEVICE=0 \
    RTLSDR_PPM=0

CMD ["/usr/local/bin/audio_bridge.py"]
