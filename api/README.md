# API Gateway

Python FastAPI gateway that sits between the frontend and the C++ search engine.

## Endpoints

| Method | Path      | Description                      |
|--------|-----------|----------------------------------|
| GET    | `/health` | Liveness / readiness probe       |
| POST   | `/search` | Search – currently returns stubs |

### POST /search (Contract 2)

```json
// Request
{"query": "fox", "k": 10}

// Response
{"results": [{"doc_id": 1, "score": 4.21, "snippet": "..."}], "latency_ms": 0}
```

## Development

```bash
pip install -r requirements.txt
uvicorn app.main:app --reload
```

## Tests

```bash
pip install -r requirements.txt
pytest tests/ -v
```

## Docker

```bash
docker compose -f infra/docker-compose.yml up api
```
