"""FastAPI gateway – search engine API layer.

Currently returns hardcoded stub results that honour Contract 2.
Once the C++ engine is live, /search will proxy to ENGINE_URL.
"""

from __future__ import annotations

import os
import time
from typing import List

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

ENGINE_URL: str = os.getenv("ENGINE_URL", "http://engine:8080")

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
# Hardcoded stub data
# ---------------------------------------------------------------------------

STUB_RESULTS: List[SearchResult] = [
    SearchResult(
        doc_id=1,
        score=4.21,
        snippet="The quick brown fox jumps over the lazy dog.",
    ),
    SearchResult(
        doc_id=42,
        score=3.05,
        snippet="Information retrieval is the science of searching.",
    ),
    SearchResult(
        doc_id=7,
        score=1.88,
        snippet="BM25 is a bag-of-words retrieval function.",
    ),
]

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------

app = FastAPI(title="Search Engine Gateway", version="0.1.0")

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
    """Return search results.

    Currently returns hardcoded stub data.
    Will proxy to the C++ engine (ENGINE_URL) once it's running.
    """
    start = time.perf_counter()

    # Stub: return up to k results from the hardcoded list
    results = STUB_RESULTS[: req.k]

    elapsed_ms = int((time.perf_counter() - start) * 1000)

    return SearchResponse(results=results, latency_ms=elapsed_ms)
