# Search Engine

A from-scratch search engine with hand-rolled inverted index, BM25 scoring, and top-k retrieval.

---

## Contracts

### Contract 1 — Document JSONL (one JSON object per line)

```json
{"doc_id": <int>, "title": <str>, "url": <str>, "text": <str>}
```

### Contract 2 — Engine Query API

```
POST /search
  request:  {"query": <str>, "k": <int>}
  response: {"results": [{"doc_id": <int>, "score": <float>, "snippet": <str>}], "latency_ms": <int>}
```

Results are sorted by `score` **DESC**; score is raw BM25.

---

## Stack

| Layer       | Technology                        | Directory |
|-------------|-----------------------------------|-----------|
| Core Engine | C++17, Crow (header-only HTTP)    | `/engine` |
| Ingestion   | Python                            | `/ingest` |
| API Gateway | Python, FastAPI                   | `/api`    |
| Frontend    | React, Vite                       | `/web`    |
| ML          | Python (embeddings, fusion, eval) | `/ml`     |
| Infra       | Docker Compose                    | `/infra`  |
| Evaluation  | Labeled query set + results       | `/eval`   |
| Database    | PostgreSQL + pgvector             | —         |

---

## Quick Start

Docker is the only prerequisite — no host Python, Node or C++ toolchain.

```bash
make run
```

On a clean checkout that fetches the corpus, builds the four images, builds the
index, and waits until every service reports healthy. First run takes a few
minutes (the engine image compiles C++17 from source and runs its test suites);
later runs take seconds.

Then open **http://localhost:5173**, or query the API directly:

```bash
make smoke
```

```json
{"results":[{"doc_id":48927588,"score":6.55,"snippet":"..."}],"latency_ms":2}
```

| Service | URL | What it is |
|---------|-----|------------|
| web | http://localhost:5173 | React + Vite frontend |
| api | http://localhost:8000/docs | FastAPI gateway, OpenAPI UI |
| engine | http://localhost:8080/health | C++ engine, Contract 2 |
| engine stats | http://localhost:8080/stats | Index and query-cache counters |
| db | `localhost:5432` | PostgreSQL + pgvector |

### Without `make`

Every target is a single command; use these directly if `make` is unavailable
(on Windows, `mingw32-make` also works):

| Target | Command |
|--------|---------|
| `make run` | `docker compose -f infra/docker-compose.yml up -d --build --wait` |
| `make build` | `docker compose -f infra/docker-compose.yml build` |
| `make corpus` | `docker compose -f infra/docker-compose.yml run --rm ingest` |
| `make index` | `docker compose -f infra/docker-compose.yml run --rm -e BUILD_ONLY=1 engine` |
| `make down` | `docker compose -f infra/docker-compose.yml down` |
| `make clean` | `docker compose -f infra/docker-compose.yml down -v` |
| `make logs` | `docker compose -f infra/docker-compose.yml logs -f` |
| `make ps` | `docker compose -f infra/docker-compose.yml ps` |
| `make stats` | `curl -s http://localhost:8080/stats` |
| `make bench` | see the Makefile — runs `bench_query` against `ingest/data/corpus.jsonl` |

`make help` lists everything.

---

## How the index gets built

The corpus is gitignored, so a clean checkout has none. `make run` fetches it
first, in a container, so no host Python is needed:

```
make corpus   →  ingest/data/corpus.jsonl   (100,000 HN stories, ~30 MB)
make index    →  engine_index volume               (meta.bin, docs.idx, docs.bin, terms.bin)
make run      →  db + engine + api + web
```

`make index` is optional. The engine builds its own index on first boot if the
volume is empty, and **loads** it on every boot after that — a load is
milliseconds against hundreds of them for a rebuild, which is the whole point of
the on-disk format. `engine/docker-entrypoint.sh` decides which happens.

To force a rebuild after the corpus changes:

```bash
make reindex
```

---

## Testing

```bash
make test          # every suite
make test-engine   # 9 C++ suites, run inside the image build
make test-api      # FastAPI gateway
make test-ingest   # ingestion, all HTTP mocked
make test-web      # frontend lint
```

The engine image runs `ctest` as a **build step**, so a failing C++ test fails
the image build and `make run` cannot start a broken engine.

---

## Configuration

Everything has a default; `.env` is optional. Copy `.env.example` to `.env` only
to override:

| Variable | Default | Purpose |
|----------|---------|---------|
| `WEB_PORT` / `API_PORT` / `ENGINE_PORT` / `POSTGRES_PORT` | 5173 / 8000 / 8080 / 5432 | Host ports |
| `POSTGRES_USER` / `POSTGRES_PASSWORD` / `POSTGRES_DB` | search / search / searchdb | Database |
| `JSONL_PATH` | `/data/corpus.jsonl` | Corpus inside the engine container |
| `INDEX_PATH` | `/index` | Index location on the engine volume |
| `BUILD_ONLY` | unset | Build the index and exit instead of serving |
| `QUERY_CACHE_CAPACITY` | 256 | Query-cache entry limit; `0` disables it |

---

## Troubleshooting

**Port already in use** — set the port in `.env` and `make run` again.

**`corpus ... does not exist`** — `make corpus`, then `make run`.

**Engine unhealthy or the API returns 502** — `make logs`. The API's `/health`
reports engine reachability separately, so `curl localhost:8000/health` tells you
which side is at fault.

**Stale results after re-fetching the corpus** — `make reindex`. The engine keeps
loading the saved index until you replace it.

**Start over completely** — `make clean` removes the index and database volumes.

See individual directory READMEs for component-specific instructions.
