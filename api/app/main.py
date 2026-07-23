"""FastAPI gateway – search engine API layer.

Proxies POST /search to the C++ engine service (ENGINE_URL) over HTTP.
Adds timeout handling and returns 502 on engine errors.
"""

from __future__ import annotations

import os
from contextlib import asynccontextmanager
from typing import List

import httpx
from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

ENGINE_URL: str = os.getenv("ENGINE_URL", "http://engine:8080")
ENGINE_TIMEOUT: float = float(os.getenv("ENGINE_TIMEOUT", "5.0"))

# ---------------------------------------------------------------------------
# Pydantic models (Contract 2)
# ---------------------------------------------------------------------------


class SearchRequest(BaseModel):
    query: str
    k: int = Field(default=10, ge=1)


class SearchResult(BaseModel):
    doc_id: int
    score: float
    snippet: str


class SearchResponse(BaseModel):
    results: List[SearchResult]
    latency_ms: int


# ---------------------------------------------------------------------------
# HTTP client (connection-pooled, reusable)
# ---------------------------------------------------------------------------

http_client = httpx.AsyncClient(timeout=ENGINE_TIMEOUT)

# ---------------------------------------------------------------------------
# Lifespan
# ---------------------------------------------------------------------------


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Manage application lifecycle — clean up httpx client on shutdown."""
    yield
    await http_client.aclose()


# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------

app = FastAPI(title="Search Engine Gateway", version="0.1.0", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

@app.get("/health")
async def health() -> dict:
    """Liveness / readiness probe."""
    return {"status": "ok"}


@app.post("/search", response_model=SearchResponse)
async def search(req: SearchRequest) -> SearchResponse:
    """Proxy search request to the C++ engine service.

    Forwards {"query": ..., "k": ...} to ENGINE_URL/search and returns the
    engine's Contract-2 response. Returns HTTP 502 on timeout or connection
    errors.
    """
    try:
        engine_resp = await http_client.post(
            f"{ENGINE_URL}/search",
            json={"query": req.query, "k": req.k},
        )
    except httpx.TimeoutException:
        raise HTTPException(
            status_code=502,
            detail="Engine request timed out",
        )
    except httpx.ConnectError:
        raise HTTPException(
            status_code=502,
            detail="Could not connect to engine service",
        )
    except httpx.HTTPError as exc:
        raise HTTPException(
            status_code=502,
            detail=f"Engine request failed: {exc}",
        )

    if engine_resp.status_code != 200:
        raise HTTPException(
            status_code=502,
            detail=f"Engine returned status {engine_resp.status_code}",
        )

    data = engine_resp.json()
    return SearchResponse(**data)
