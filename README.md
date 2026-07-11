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

```bash
# Build and run all services
docker compose -f infra/docker-compose.yml up --build
```

See individual directory READMEs for component-specific instructions.
