# Infra

Docker Compose configuration for the whole stack. Run it from the repository
root — see the [Quick Start](../README.md#quick-start), or `make help`.

```bash
docker compose -f infra/docker-compose.yml up -d --build --wait
```

## Services

| Service | Image | Port | Notes |
|---------|-------|------|-------|
| `engine` | built from `../engine` | 8080 | C++17 + Crow. Healthchecked on `/health`. |
| `api` | built from `../api` | 8000 | FastAPI gateway. Starts only once the engine is healthy. |
| `web` | built from `../web` | 5173 | Vite dev server. |
| `db` | `pgvector/pgvector:pg16` | 5432 | Healthchecked with `pg_isready`. |
| `ingest` | built from `../ingest` | — | One-shot corpus fetch. In the `tools` profile, so `up` never runs it. |

## Volumes

| Volume | Holds | Removed by |
|--------|-------|------------|
| `engine_index` | the on-disk index (`meta.bin`, `docs.idx`, `docs.bin`, `terms.bin`) | `make clean`, `make reindex` |
| `pgdata` | PostgreSQL data directory | `make clean` |

`ingest/data` is a bind mount rather than a volume: the corpus is generated on
the host by `make corpus` and mounted read-only into the engine.

## Two things worth knowing

**The engine loads rather than rebuilds.** Both `JSONL_PATH` and `INDEX_PATH` are
set, and `engine/docker-entrypoint.sh` drops `JSONL_PATH` once an index exists on
the volume — so the corpus is parsed once and later boots just load it. `make
reindex` deletes the volume to force a rebuild.

**Startup order is health-gated, not merely ordered.** `api` uses
`depends_on: condition: service_healthy`, so it waits for the engine to actually
answer `/health` rather than just to have started. Without that, the first query
after `up` can race the index load and come back 502.

## Defaults

Every value has an inline default and `../.env` is declared `required: false`, so
a clean checkout with no `.env` comes up unchanged. Copy `.env.example` to `.env`
to override ports or database credentials.
