"""Tests for the FastAPI gateway.

Mocks the httpx client to simulate engine responses without a running engine.
Covers pagination, validation, error handling, and health checks.
"""

from __future__ import annotations

from unittest.mock import AsyncMock, patch

import httpx
import pytest
from fastapi.testclient import TestClient

from app.main import app


@pytest.fixture()
def client() -> TestClient:
    return TestClient(app)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

ENGINE_RESPONSE = {
    "results": [
        {"doc_id": 1, "score": 4.21, "snippet": "The quick brown fox jumps over the lazy dog."},
        {"doc_id": 42, "score": 3.05, "snippet": "Information retrieval is the science of searching."},
        {"doc_id": 7, "score": 1.88, "snippet": "BM25 is a bag-of-words retrieval function."},
    ],
    "latency_ms": 2,
}


def _mock_engine_response(
    status_code: int = 200,
    json_data: dict | None = None,
) -> httpx.Response:
    """Build a fake httpx.Response."""
    data = json_data if json_data is not None else ENGINE_RESPONSE
    return httpx.Response(
        status_code=status_code,
        json=data,
        request=httpx.Request("POST", "http://engine:8080/search"),
    )


def _mock_health_response(
    status_code: int = 200,
) -> httpx.Response:
    """Build a fake httpx.Response for health."""
    return httpx.Response(
        status_code=status_code,
        json={"status": "ok"},
        request=httpx.Request("GET", "http://engine:8080/health"),
    )


# ── Health ──────────────────────────────────────────────────────────────────


@patch("app.main.http_client")
def test_health_returns_ok_when_engine_up(mock_client, client: TestClient) -> None:
    mock_client.get = AsyncMock(return_value=_mock_health_response())
    resp = client.get("/health")
    assert resp.status_code == 200
    assert resp.json() == {"status": "ok", "engine": "reachable"}


@patch("app.main.http_client")
def test_health_reports_engine_down(mock_client, client: TestClient) -> None:
    mock_client.get = AsyncMock(side_effect=httpx.ConnectError("refused"))
    resp = client.get("/health")
    assert resp.status_code == 200
    assert resp.json() == {"status": "ok", "engine": "unreachable (connection refused)"}


# ── Search – Contract 2 compliance ─────────────────────────────────────────


@patch("app.main.http_client")
def test_search_returns_contract2_shape(mock_client, client: TestClient) -> None:
    """Response must have 'results' (list) and 'latency_ms' (int)."""
    mock_client.post = AsyncMock(return_value=_mock_engine_response())

    resp = client.post("/search", json={"query": "fox", "k": 10})
    assert resp.status_code == 200

    body = resp.json()
    assert "results" in body
    assert "latency_ms" in body
    assert isinstance(body["results"], list)
    assert isinstance(body["latency_ms"], int)


@patch("app.main.http_client")
def test_search_result_fields(mock_client, client: TestClient) -> None:
    """Each result must contain doc_id (int), score (float), snippet (str)."""
    mock_client.post = AsyncMock(return_value=_mock_engine_response())

    resp = client.post("/search", json={"query": "fox", "k": 10})
    results = resp.json()["results"]
    assert len(results) > 0

    for r in results:
        assert isinstance(r["doc_id"], int)
        assert isinstance(r["score"], (int, float))
        assert isinstance(r["snippet"], str)


# ── Validation ─────────────────────────────────────────────────────────────


def test_search_missing_query_returns_422(client: TestClient) -> None:
    """Missing required field 'query' should yield 422."""
    resp = client.post("/search", json={"k": 5})
    assert resp.status_code == 422
    assert "error" in resp.json()
    assert "Validation error" in resp.json()["error"]


def test_search_empty_query_returns_422(client: TestClient) -> None:
    """Blank query should yield 422."""
    resp = client.post("/search", json={"query": "   ", "k": 5})
    assert resp.status_code == 422
    assert "must not be empty" in resp.json()["detail"].lower()


def test_search_invalid_k_bounds(client: TestClient) -> None:
    """k < 1 or k > 100 should yield 422."""
    resp = client.post("/search", json={"query": "test", "k": 0})
    assert resp.status_code == 422
    
    resp = client.post("/search", json={"query": "test", "k": 101})
    assert resp.status_code == 422


# ── Pagination ─────────────────────────────────────────────────────────────


@patch("app.main.http_client")
def test_search_pagination_requests_k(mock_client, client: TestClient) -> None:
    """Check that gateway requests page * per_page results from engine."""
    mock_client.post = AsyncMock(return_value=_mock_engine_response())
    
    client.post("/search", json={"query": "hello", "page": 2, "per_page": 5})
    
    mock_client.post.assert_called_once()
    kwargs = mock_client.post.call_args.kwargs
    assert kwargs["json"]["query"] == "hello"
    assert kwargs["json"]["k"] == 10  # 2 * 5


@patch("app.main.http_client")
def test_search_pagination_slices_results(mock_client, client: TestClient) -> None:
    """Check that gateway slices the results down to the correct page window."""
    # Engine returns 3 results
    mock_client.post = AsyncMock(return_value=_mock_engine_response())
    
    # Request page 2 with per_page 2
    # k requested will be 4. Engine returns 3.
    # Gateway should slice offset 2 : 4 -> which is just the 3rd result.
    resp = client.post("/search", json={"query": "hello", "page": 2, "per_page": 2})
    assert resp.status_code == 200
    body = resp.json()
    assert body["page"] == 2
    assert body["per_page"] == 2
    assert body["total_results"] == 3
    assert len(body["results"]) == 1
    assert body["results"][0]["doc_id"] == 7


# ── Proxy error handling ───────────────────────────────────────────────────


@patch("app.main.http_client")
def test_search_engine_timeout_returns_502(mock_client, client: TestClient) -> None:
    """Engine timeout should return 502 with structured body."""
    mock_client.post = AsyncMock(side_effect=httpx.TimeoutException("timed out"))

    resp = client.post("/search", json={"query": "slow", "k": 5})
    assert resp.status_code == 502
    assert "error" in resp.json()
    assert "timed out" in resp.json()["error"].lower()


@patch("app.main.http_client")
def test_search_engine_connect_error_returns_502(mock_client, client: TestClient) -> None:
    """Engine connection error should return 502."""
    mock_client.post = AsyncMock(side_effect=httpx.ConnectError("refused"))

    resp = client.post("/search", json={"query": "down", "k": 5})
    assert resp.status_code == 502
    assert "connect" in resp.json()["error"].lower()


@patch("app.main.http_client")
def test_search_engine_non_200_returns_502(mock_client, client: TestClient) -> None:
    """Engine returning non-200 should surface as 502."""
    mock_client.post = AsyncMock(return_value=_mock_engine_response(status_code=500))

    resp = client.post("/search", json={"query": "error", "k": 5})
    assert resp.status_code == 502
    assert "500" in resp.json()["error"]


# ── Request logging ────────────────────────────────────────────────────────


def test_request_logging_adds_request_id(client: TestClient) -> None:
    resp = client.post("/search", json={"query": "test"})
    assert "X-Request-ID" in resp.headers
