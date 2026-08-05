FROM gcc:12 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake git ca-certificates libasio-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DASIO_INCLUDE_DIR=/usr/include \
    && cmake --build build --parallel "$(nproc)" \
    && cmake --build build --target engine_tests tokenizer_tests index_tests bm25_tests topk_tests persist_tests docstore_tests query_tests \
    && cd build && ctest --output-on-failure

# ── Runtime stage ──────────────────────────────────────────────────────────
FROM debian:bookworm-slim

# curl is here for the container healthcheck, which is what lets the API wait
# until the engine has finished loading its index rather than racing it.
RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/search_engine /usr/local/bin/search_engine
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

# Default index location; compose mounts a named volume here so the index
# survives restarts and only gets built once.
ENV INDEX_PATH=/index
RUN mkdir -p /index

EXPOSE 8080

HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=12 \
    CMD curl -fsS http://localhost:8080/health || exit 1

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD ["search_engine"]
