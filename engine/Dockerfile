FROM gcc:12 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake git ca-certificates libasio-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DASIO_INCLUDE_DIR=/usr/include \
    && cmake --build build --parallel "$(nproc)" \
    && cmake --build build --target engine_tests tokenizer_tests index_tests bm25_tests topk_tests persist_tests docstore_tests query_tests cache_tests \
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
# Strip CR before chmod. .gitattributes pins this file to LF, but a ZIP download
# or a clone made before that existed can still arrive with CRLF — and then the
# shebang reads "#!/bin/sh\r", so the kernel hunts for an interpreter named
# "/bin/sh\r" and reports "no such file or directory" against the script itself.
RUN sed -i 's/\r$//' /usr/local/bin/docker-entrypoint.sh \
    && chmod +x /usr/local/bin/docker-entrypoint.sh

# Default index location; compose mounts a named volume here so the index
# survives restarts and only gets built once.
ENV INDEX_PATH=/index
RUN mkdir -p /index

EXPOSE 8080

# start-period covers a cold first boot, which builds the index before serving.
# On the 100k corpus that is ~9 s of indexing plus a save that reads every
# document back from /data — a bind mount, so on Docker Desktop for Windows it
# runs ~30x slower than native and the whole boot can take a couple of minutes.
# Failures during start-period do not count toward retries, so a genuinely dead
# engine is still caught within 60 s once it is over.
HEALTHCHECK --interval=5s --timeout=3s --start-period=300s --retries=12 \
    CMD curl -fsS http://localhost:8080/health || exit 1

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD ["search_engine"]
