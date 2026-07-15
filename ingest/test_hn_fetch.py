"""Tests for hn_fetch — mapping, dedup, write, and fetch logic.

Uses unittest.mock to avoid hitting the real Algolia API.
"""

from __future__ import annotations

import json
import textwrap
from pathlib import Path
from typing import Any
from unittest.mock import MagicMock, patch

import pytest

from hn_fetch import (
    fetch_stories,
    hit_to_doc,
    write_jsonl,
)


# ── hit_to_doc ───────────────────────────────────────────────────────────────

class TestHitToDoc:
    """Unit tests for the Algolia-hit → Contract-1 mapping."""

    def _make_hit(self, **overrides: Any) -> dict[str, Any]:
        base: dict[str, Any] = {
            "objectID": "12345",
            "title": "Show HN: My Project",
            "url": "https://example.com",
            "story_text": "",
            "comment_text": "",
        }
        base.update(overrides)
        return base

    def test_basic_mapping(self):
        doc = hit_to_doc(self._make_hit())
        assert doc is not None
        assert doc["doc_id"] == 12345
        assert doc["title"] == "Show HN: My Project"
        assert doc["url"] == "https://example.com"
        assert doc["text"] == "Show HN: My Project"

    def test_text_includes_story_text(self):
        doc = hit_to_doc(self._make_hit(story_text="Some body text."))
        assert doc is not None
        assert "Some body text." in doc["text"]
        assert doc["text"].startswith("Show HN: My Project")

    def test_text_falls_back_to_comment_text(self):
        doc = hit_to_doc(self._make_hit(story_text="", comment_text="A comment."))
        assert doc is not None
        assert "A comment." in doc["text"]

    def test_story_text_takes_precedence_over_comment_text(self):
        doc = hit_to_doc(
            self._make_hit(story_text="Story body.", comment_text="Comment body.")
        )
        assert doc is not None
        assert "Story body." in doc["text"]
        assert "Comment body." not in doc["text"]

    def test_url_fallback_to_hn_item(self):
        doc = hit_to_doc(self._make_hit(url=""))
        assert doc is not None
        assert doc["url"] == "https://news.ycombinator.com/item?id=12345"

    def test_url_fallback_when_none(self):
        doc = hit_to_doc(self._make_hit(url=None))
        assert doc is not None
        assert doc["url"] == "https://news.ycombinator.com/item?id=12345"

    def test_missing_object_id_returns_none(self):
        hit = self._make_hit()
        del hit["objectID"]
        assert hit_to_doc(hit) is None

    def test_non_numeric_object_id_returns_none(self):
        assert hit_to_doc(self._make_hit(objectID="abc")) is None

    def test_empty_title_and_no_text_returns_none(self):
        assert hit_to_doc(self._make_hit(title="", story_text="", comment_text="")) is None

    def test_whitespace_stripping(self):
        doc = hit_to_doc(self._make_hit(title="  Hello  ", url="  https://x.com  "))
        assert doc is not None
        assert doc["title"] == "Hello"
        assert doc["url"] == "https://x.com"


# ── write_jsonl ──────────────────────────────────────────────────────────────

class TestWriteJsonl:
    def test_writes_valid_jsonl(self, tmp_path: Path):
        docs = [
            {"doc_id": 1, "title": "A", "url": "https://a.com", "text": "A"},
            {"doc_id": 2, "title": "B", "url": "https://b.com", "text": "B"},
        ]
        out = tmp_path / "corpus.jsonl"
        write_jsonl(docs, out)

        lines = out.read_text(encoding="utf-8").strip().splitlines()
        assert len(lines) == 2
        for line in lines:
            obj = json.loads(line)
            assert set(obj.keys()) == {"doc_id", "title", "url", "text"}

    def test_creates_parent_dirs(self, tmp_path: Path):
        out = tmp_path / "sub" / "dir" / "corpus.jsonl"
        write_jsonl([{"doc_id": 1, "title": "T", "url": "u", "text": "t"}], out)
        assert out.exists()

    def test_empty_docs(self, tmp_path: Path):
        out = tmp_path / "empty.jsonl"
        write_jsonl([], out)
        assert out.read_text(encoding="utf-8") == ""


# ── fetch_stories ────────────────────────────────────────────────────────────

def _make_algolia_response(
    hits: list[dict[str, Any]], page: int = 0
) -> dict[str, Any]:
    return {"hits": hits, "page": page, "nbPages": 500, "hitsPerPage": 20}


class TestFetchStories:
    """Integration-level tests for fetch_stories with mocked HTTP."""

    def _mock_hit(self, oid: int, title: str = "Title") -> dict[str, Any]:
        return {
            "objectID": str(oid),
            "title": title,
            "url": f"https://example.com/{oid}",
            "story_text": "",
        }

    def test_fetches_until_target(self):
        """Should stop once target count is reached."""
        hits = [self._mock_hit(i) for i in range(20)]
        mock_resp = MagicMock()
        mock_resp.status_code = 200
        mock_resp.json.return_value = _make_algolia_response(hits)

        session = MagicMock()
        session.get.return_value = mock_resp

        docs = fetch_stories(target=5, session=session, delay=0)
        assert len(docs) == 5
        # Only one page needed
        assert session.get.call_count == 1

    def test_deduplicates(self):
        """Duplicate objectIDs across pages should be dropped."""
        # Page 0 and page 1 both return the same IDs
        hits = [self._mock_hit(i) for i in range(3)]
        mock_resp = MagicMock()
        mock_resp.status_code = 200
        mock_resp.json.return_value = _make_algolia_response(hits)

        call_count = 0

        def side_effect(*args, **kwargs):
            nonlocal call_count
            call_count += 1
            if call_count > 10:
                # Safety: prevent infinite loop in test
                empty = MagicMock()
                empty.status_code = 200
                empty.json.return_value = _make_algolia_response([])
                return empty
            return mock_resp

        session = MagicMock()
        session.get.side_effect = side_effect

        docs = fetch_stories(target=100, session=session, delay=0)
        # Only 3 unique IDs, so at most 3 docs
        assert len(docs) == 3

    def test_handles_http_error(self):
        """Non-200 should stop fetching gracefully."""
        mock_resp = MagicMock()
        mock_resp.status_code = 503
        session = MagicMock()
        session.get.return_value = mock_resp

        docs = fetch_stories(target=10, session=session, delay=0)
        assert docs == []

    def test_handles_empty_hits(self):
        """Empty hits list should stop fetching."""
        mock_resp = MagicMock()
        mock_resp.status_code = 200
        mock_resp.json.return_value = _make_algolia_response([])

        session = MagicMock()
        session.get.return_value = mock_resp

        docs = fetch_stories(target=10, session=session, delay=0)
        assert docs == []
