FROM gcc:12 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake git ca-certificates libasio-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DASIO_INCLUDE_DIR=/usr/include \
    && cmake --build build --parallel "$(nproc)" \
    && cmake --build build --target engine_tests \
    && cd build && ctest --output-on-failure

# ── Runtime stage ──────────────────────────────────────────────────────────
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/search_engine /usr/local/bin/search_engine

EXPOSE 8080

CMD ["search_engine"]
