"""Tests for the FastAPI gateway.

Mocks the httpx client to simulate engine responses without a running engine.
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


# ── Health ──────────────────────────────────────────────────────────────────


def test_health_returns_ok(client: TestClient) -> None:
    resp = client.get("/health")
    assert resp.status_code == 200
    assert resp.json() == {"status": "ok"}


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


@patch("app.main.http_client")
def test_search_results_sorted_desc(mock_client, client: TestClient) -> None:
    """Results must be sorted by score descending."""
    mock_client.post = AsyncMock(return_value=_mock_engine_response())

    resp = client.post("/search", json={"query": "fox", "k": 10})
    scores = [r["score"] for r in resp.json()["results"]]
    assert scores == sorted(scores, reverse=True)


def test_search_missing_query_returns_422(client: TestClient) -> None:
    """Missing required field 'query' should yield 422."""
    resp = client.post("/search", json={"k": 5})
    assert resp.status_code == 422


@patch("app.main.http_client")
def test_search_default_k(mock_client, client: TestClient) -> None:
    """Omitting k should default to 10 and still work."""
    mock_client.post = AsyncMock(return_value=_mock_engine_response())

    resp = client.post("/search", json={"query": "test"})
    assert resp.status_code == 200
    assert len(resp.json()["results"]) <= 10


# ── Proxy error handling ───────────────────────────────────────────────────


@patch("app.main.http_client")
def test_search_engine_timeout_returns_502(mock_client, client: TestClient) -> None:
    """Engine timeout should return 502."""
    mock_client.post = AsyncMock(side_effect=httpx.TimeoutException("timed out"))

    resp = client.post("/search", json={"query": "slow", "k": 5})
    assert resp.status_code == 502
    assert "timed out" in resp.json()["detail"].lower()


@patch("app.main.http_client")
def test_search_engine_connect_error_returns_502(mock_client, client: TestClient) -> None:
    """Engine connection error should return 502."""
    mock_client.post = AsyncMock(side_effect=httpx.ConnectError("refused"))

    resp = client.post("/search", json={"query": "down", "k": 5})
    assert resp.status_code == 502
    assert "connect" in resp.json()["detail"].lower()


@patch("app.main.http_client")
def test_search_engine_non_200_returns_502(mock_client, client: TestClient) -> None:
    """Engine returning non-200 should surface as 502."""
    mock_client.post = AsyncMock(return_value=_mock_engine_response(status_code=500))

    resp = client.post("/search", json={"query": "error", "k": 5})
    assert resp.status_code == 502
    assert "500" in resp.json()["detail"]


@patch("app.main.http_client")
def test_search_forwards_query_and_k(mock_client, client: TestClient) -> None:
    """Verify the gateway forwards query and k to the engine."""
    mock_client.post = AsyncMock(return_value=_mock_engine_response())

    client.post("/search", json={"query": "hello world", "k": 3})

    # Check the call was made with correct arguments
    mock_client.post.assert_called_once()
    call_kwargs = mock_client.post.call_args
    assert call_kwargs.kwargs["json"]["query"] == "hello world"
    assert call_kwargs.kwargs["json"]["k"] == 3
