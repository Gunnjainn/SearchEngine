#!/bin/sh
# ---------------------------------------------------------------------------
# Decide whether the engine builds an index or loads one.
#
# main() rebuilds whenever JSONL_PATH is set, so to load a saved index we have
# to drop that variable. The effect:
#
#   first boot   — no index on the volume, build from the corpus and save it
#   later boots  — index found, load it (milliseconds instead of a full rebuild)
#   BUILD_ONLY   — always rebuild, because that is the point of `make index`
# ---------------------------------------------------------------------------
set -e

if [ -n "${BUILD_ONLY}" ] && [ "${BUILD_ONLY}" != "0" ]; then
    echo "[entrypoint] BUILD_ONLY: rebuilding the index from ${JSONL_PATH:-<unset>}"
elif [ -n "${INDEX_PATH}" ] && [ -f "${INDEX_PATH}/meta.bin" ]; then
    echo "[entrypoint] index found at ${INDEX_PATH} — loading it, not rebuilding"
    unset JSONL_PATH
else
    echo "[entrypoint] no index at ${INDEX_PATH:-<unset>} — building from ${JSONL_PATH:-<unset>}"
    if [ -n "${JSONL_PATH}" ] && [ ! -f "${JSONL_PATH}" ]; then
        echo "[entrypoint] ERROR: corpus ${JSONL_PATH} does not exist." >&2
        echo "[entrypoint] Run 'make corpus' to fetch it, then try again." >&2
        exit 1
    fi
fi

exec "$@"
