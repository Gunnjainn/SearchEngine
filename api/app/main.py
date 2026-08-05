"""FastAPI gateway – search engine API layer.

Proxies POST /search to the C++ engine service (ENGINE_URL) over HTTP.
Adds pagination, input validation, structured error responses, request
logging, and a /health endpoint that verifies engine reachability.
"""

from __future__ import annotations

import logging
import os
import time
from contextlib import asynccontextmanager
from typing import List, Optional
from uuid import uuid4

import httpx
from fastapi import FastAPI, HTTPException, Request
from fastapi.exceptions import RequestValidationError
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field, field_validator

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

ENGINE_URL: str = os.getenv("ENGINE_URL", "http://engine:8080")
ENGINE_TIMEOUT: float = float(os.getenv("ENGINE_TIMEOUT", "5.0"))

K_MIN: int = 1
K_MAX: int = 100
PAGE_SIZE_DEFAULT: int = 10
PAGE_SIZE_MAX: int = 100

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s %(message)s",
)
logger = logging.getLogger("api.gateway")

# ---------------------------------------------------------------------------
# Pydantic models (Contract 2)
# ---------------------------------------------------------------------------


class SearchRequest(BaseModel):
    """Inbound search request with pagination support.

    Clients may use either:
      - ``k`` directly (engine-native), or
      - ``page`` + ``per_page`` for paginated access.

    If ``page`` is supplied, ``k`` is computed as ``page * per_page`` so the
    engine returns enough results to fill the requested page.  The gateway
    then slices to the correct page window before responding.
    """

    query: str
    k: int = Field(default=10, ge=K_MIN, le=K_MAX)
    page: Optional[int] = Field(default=None, ge=1)
    per_page: Optional[int] = Field(default=None, ge=1, le=PAGE_SIZE_MAX)

    @field_validator("query")
    @classmethod
    def query_must_not_be_blank(cls, v: str) -> str:
        if not v or not v.strip():
            raise ValueError("query must not be empty or whitespace-only")
        return v


class SearchResult(BaseModel):
    doc_id: int
    score: float
    snippet: str


class SearchResponse(BaseModel):
    results: List[SearchResult]
    latency_ms: int
    page: Optional[int] = None
    per_page: Optional[int] = None
    total_results: Optional[int] = None


class ErrorResponse(BaseModel):
    """Structured error body returned for all 4xx / 5xx responses."""

    error: str
    detail: Optional[str] = None


class HealthResponse(BaseModel):
    status: str
    engine: str


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

app = FastAPI(title="Search Engine Gateway", version="0.2.0", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


# ---------------------------------------------------------------------------
# Structured error handlers
# ---------------------------------------------------------------------------


@app.exception_handler(RequestValidationError)
async def validation_error_handler(
    request: Request, exc: RequestValidationError
) -> JSONResponse:
    """Return a clean 422 with structured body for validation errors."""
    messages: list[str] = []
    for err in exc.errors():
        loc = " -> ".join(str(l) for l in err["loc"] if l != "body")
        messages.append(f"{loc}: {err['msg']}" if loc else err["msg"])
    return JSONResponse(
        status_code=422,
        content=ErrorResponse(
            error="Validation error",
            detail="; ".join(messages),
        ).model_dump(),
    )


@app.exception_handler(HTTPException)
async def http_exception_handler(
    request: Request, exc: HTTPException
) -> JSONResponse:
    """Wrap HTTPException in our structured ErrorResponse format."""
    return JSONResponse(
        status_code=exc.status_code,
        content=ErrorResponse(
            error=exc.detail if isinstance(exc.detail, str) else str(exc.detail),
        ).model_dump(),
    )


# ---------------------------------------------------------------------------
# Request logging middleware
# ---------------------------------------------------------------------------


@app.middleware("http")
async def request_logging_middleware(request: Request, call_next):
    """Log every request with method, path, status, and duration."""
    request_id = uuid4().hex[:8]
    start = time.monotonic()
    logger.info(
        "req_start request_id=%s method=%s path=%s",
        request_id,
        request.method,
        request.url.path,
    )
    response = await call_next(request)
    elapsed_ms = (time.monotonic() - start) * 1000
    logger.info(
        "req_end   request_id=%s status=%d duration_ms=%.1f",
        request_id,
        response.status_code,
        elapsed_ms,
    )
    response.headers["X-Request-ID"] = request_id
    return response


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------


@app.get("/health", response_model=HealthResponse)
async def health() -> HealthResponse:
    """Liveness / readiness probe.  Also checks engine reachability."""
    engine_status = "reachable"
    try:
        resp = await http_client.get(f"{ENGINE_URL}/health")
        if resp.status_code != 200:
            engine_status = f"unhealthy (HTTP {resp.status_code})"
    except httpx.TimeoutException:
        engine_status = "unreachable (timeout)"
    except httpx.ConnectError:
        engine_status = "unreachable (connection refused)"
    except httpx.HTTPError:
        engine_status = "unreachable"

    return HealthResponse(status="ok", engine=engine_status)


@app.post("/search", response_model=SearchResponse)
async def search(req: SearchRequest) -> SearchResponse:
    """Proxy search request to the C++ engine service.

    Supports pagination via ``page`` / ``per_page`` query parameters.
    When pagination params are present the gateway requests enough results
    from the engine (``k = page * per_page``) and slices to the page window.

    Forwards ``score`` and ``latency_ms`` from the engine verbatim.
    """
    # ── Resolve pagination ──────────────────────────────────────────────
    paginating = req.page is not None
    if paginating:
        per_page = req.per_page if req.per_page is not None else PAGE_SIZE_DEFAULT
        engine_k = min(req.page * per_page, K_MAX)
        offset = (req.page - 1) * per_page
    else:
        engine_k = req.k
        per_page = None
        offset = 0

    # ── Forward to engine ───────────────────────────────────────────────
    try:
        engine_resp = await http_client.post(
            f"{ENGINE_URL}/search",
            json={"query": req.query, "k": engine_k},
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

    # ── Slice for pagination ────────────────────────────────────────────
    all_results = data.get("results", [])
    total = len(all_results)
    if paginating:
        page_results = all_results[offset : offset + per_page]
    else:
        page_results = all_results

    return SearchResponse(
        results=[SearchResult(**r) for r in page_results],
        latency_ms=data.get("latency_ms", 0),
        page=req.page if paginating else None,
        per_page=per_page if paginating else None,
        total_results=total if paginating else None,
    )
