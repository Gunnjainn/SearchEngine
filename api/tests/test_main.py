"""Tests for the FastAPI gateway."""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from app.main import app, STUB_RESULTS


@pytest.fixture()
def client() -> TestClient:
    return TestClient(app)


# ── Health ──────────────────────────────────────────────────────────────────


def test_health_returns_ok(client: TestClient) -> None:
    resp = client.get("/health")
    assert resp.status_code == 200
    assert resp.json() == {"status": "ok"}


# ── Search – Contract 2 compliance ─────────────────────────────────────────


def test_search_returns_contract2_shape(client: TestClient) -> None:
    """Response must have 'results' (list) and 'latency_ms' (int)."""
    resp = client.post("/search", json={"query": "fox", "k": 10})
    assert resp.status_code == 200

    body = resp.json()
    assert "results" in body
    assert "latency_ms" in body
    assert isinstance(body["results"], list)
    assert isinstance(body["latency_ms"], int)


def test_search_result_fields(client: TestClient) -> None:
    """Each result must contain doc_id (int), score (float), snippet (str)."""
    resp = client.post("/search", json={"query": "fox", "k": 10})
    results = resp.json()["results"]
    assert len(results) > 0

    for r in results:
        assert isinstance(r["doc_id"], int)
        assert isinstance(r["score"], (int, float))
        assert isinstance(r["snippet"], str)


def test_search_results_sorted_desc(client: TestClient) -> None:
    """Results must be sorted by score descending."""
    resp = client.post("/search", json={"query": "fox", "k": 10})
    scores = [r["score"] for r in resp.json()["results"]]
    assert scores == sorted(scores, reverse=True)


def test_search_respects_k(client: TestClient) -> None:
    """Requesting k=1 must return at most 1 result."""
    resp = client.post("/search", json={"query": "fox", "k": 1})
    results = resp.json()["results"]
    assert len(results) == 1


def test_search_returns_stub_data(client: TestClient) -> None:
    """Stub should return the known hardcoded results."""
    resp = client.post("/search", json={"query": "anything", "k": 10})
    results = resp.json()["results"]
    assert len(results) == len(STUB_RESULTS)
    assert results[0]["doc_id"] == 1
    assert results[1]["doc_id"] == 42
    assert results[2]["doc_id"] == 7


def test_search_missing_query_returns_422(client: TestClient) -> None:
    """Missing required field 'query' should yield 422."""
    resp = client.post("/search", json={"k": 5})
    assert resp.status_code == 422


def test_search_default_k(client: TestClient) -> None:
    """Omitting k should default to 10 and still work."""
    resp = client.post("/search", json={"query": "test"})
    assert resp.status_code == 200
    assert len(resp.json()["results"]) <= 10
