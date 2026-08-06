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
    HITS_PER_PAGE,
    fetch_stories,
    hit_to_doc,
    iter_stories,
    write_jsonl,
    write_jsonl_stream,
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


# ── time-cursor windowing ────────────────────────────────────────────────────

class TestCursorWindowing:
    """The scaling mechanism.

    Algolia caps any single query at 1000 results, so paging cannot go deeper.
    Getting past that means re-querying with created_at_i < the oldest result
    seen so far. These tests pin that behaviour and, just as importantly, every
    way the walk must terminate rather than spin forever.
    """

    def _hit(self, oid: int, created_at: int) -> dict[str, Any]:
        return {
            "objectID": str(oid),
            "title": f"Story {oid}",
            "url": f"https://example.com/{oid}",
            "story_text": "",
            "created_at_i": created_at,
        }

    def _session_returning(self, windows: list[list[dict[str, Any]]]) -> MagicMock:
        """A session whose successive calls return the given windows, then empty."""
        responses = []
        for hits in windows + [[]]:
            resp = MagicMock()
            resp.status_code = 200
            resp.json.return_value = {"hits": hits, "page": 0, "nbPages": 1}
            responses.append(resp)
        session = MagicMock()
        session.get.side_effect = responses
        return session

    def test_first_request_has_no_cursor(self):
        session = self._session_returning([[self._hit(1, 500)]])
        list(iter_stories(target=1, session=session, delay=0))

        _, kwargs = session.get.call_args_list[0]
        assert "numericFilters" not in kwargs["params"]

    def test_second_request_filters_below_the_oldest_seen(self):
        window1 = [self._hit(1, 900), self._hit(2, 800), self._hit(3, 700)]
        window2 = [self._hit(4, 600)]
        session = self._session_returning([window1, window2])

        list(iter_stories(target=4, session=session, delay=0))

        _, kwargs = session.get.call_args_list[1]
        # 700 was the oldest of the first window, so the next window must start
        # strictly below it or documents would be fetched twice.
        assert kwargs["params"]["numericFilters"] == "created_at_i<700"

    def test_walks_past_the_thousand_result_cap(self):
        # Two full windows: more documents than any single Algolia query returns.
        window1 = [self._hit(i, 10_000 - i) for i in range(1000)]
        window2 = [self._hit(1000 + i, 9_000 - i) for i in range(500)]
        session = self._session_returning([window1, window2])

        docs = list(iter_stories(target=1500, session=session, delay=0))

        assert len(docs) == 1500
        assert len({d["doc_id"] for d in docs}) == 1500  # no duplicates across windows

    def test_stops_when_cursor_cannot_advance(self):
        # Every window reports the same oldest timestamp, so the walk cannot
        # move; without a guard this would request forever.
        same = [self._hit(1, 500), self._hit(2, 500)]
        other = [self._hit(3, 500), self._hit(4, 500)]
        session = self._session_returning([same, other, other, other])

        docs = list(iter_stories(target=1000, session=session, delay=0))

        assert len(docs) < 1000
        assert session.get.call_count <= 3

    def test_stops_when_hits_have_no_timestamp(self):
        # Hits without created_at_i give nothing to page on.
        hit = {"objectID": "1", "title": "T", "url": "u", "story_text": ""}
        session = self._session_returning([[hit]])

        docs = list(iter_stories(target=100, session=session, delay=0))

        assert len(docs) == 1
        assert session.get.call_count == 1

    def test_stops_when_a_window_is_all_duplicates(self):
        window = [self._hit(1, 900), self._hit(2, 800)]
        # The same documents come back with an older cursor — no progress.
        session = self._session_returning([window, window, window])

        docs = list(iter_stories(target=100, session=session, delay=0))

        assert len(docs) == 2
        assert session.get.call_count <= 3

    def test_target_smaller_than_a_page_asks_for_less(self):
        session = self._session_returning([[self._hit(i, 100 - i) for i in range(5)]])
        list(iter_stories(target=5, session=session, delay=0))

        _, kwargs = session.get.call_args_list[0]
        assert kwargs["params"]["hitsPerPage"] == 5

    def test_large_target_uses_the_full_page_size(self):
        session = self._session_returning([[self._hit(i, 100_000 - i) for i in range(10)]])
        list(iter_stories(target=50_000, session=session, delay=0))

        _, kwargs = session.get.call_args_list[0]
        assert kwargs["params"]["hitsPerPage"] == HITS_PER_PAGE


    def test_before_pins_the_first_window(self):
        """Without --before the crawl starts at 'now' and is unreproducible."""
        session = self._session_returning([[self._hit(1, 500)]])
        list(iter_stories(target=1, session=session, delay=0, before=1_700_000_000))

        _, kwargs = session.get.call_args_list[0]
        assert kwargs["params"]["numericFilters"] == "created_at_i<1700000000"

    def test_before_still_advances_on_later_windows(self):
        window1 = [self._hit(1, 900), self._hit(2, 800)]
        window2 = [self._hit(3, 700)]
        session = self._session_returning([window1, window2])

        list(iter_stories(target=3, session=session, delay=0, before=1000))

        first = session.get.call_args_list[0][1]["params"]["numericFilters"]
        second = session.get.call_args_list[1][1]["params"]["numericFilters"]
        assert first == "created_at_i<1000"   # the pin
        assert second == "created_at_i<800"   # then the oldest seen

    def test_is_lazy(self):
        """A generator, so a caller can stop early without fetching everything."""
        window = [self._hit(i, 1000 - i) for i in range(1000)]
        session = self._session_returning([window, window])

        gen = iter_stories(target=1000, session=session, delay=0)
        next(gen)

        # One document consumed must not have driven more than one request.
        assert session.get.call_count == 1


# ── streaming writer ─────────────────────────────────────────────────────────

class TestWriteJsonlStream:

    def test_writes_from_an_iterator_and_counts(self, tmp_path: Path):
        out = tmp_path / "corpus.jsonl"

        def gen():
            for i in range(3):
                yield {"doc_id": i, "title": f"T{i}", "url": "u", "text": "x"}

        assert write_jsonl_stream(gen(), out) == 3
        lines = out.read_text(encoding="utf-8").strip().split("\n")
        assert len(lines) == 3
        assert json.loads(lines[0])["doc_id"] == 0

    def test_does_not_materialise_the_input(self, tmp_path: Path):
        """Documents are written as they arrive, not collected first."""
        out = tmp_path / "corpus.jsonl"
        seen = []

        def gen():
            for i in range(5):
                seen.append(i)
                yield {"doc_id": i, "title": "T", "url": "u", "text": "x"}

        written = write_jsonl_stream(gen(), out)
        assert written == 5
        assert seen == [0, 1, 2, 3, 4]

    def test_empty_iterator_writes_an_empty_file(self, tmp_path: Path):
        out = tmp_path / "corpus.jsonl"
        assert write_jsonl_stream(iter([]), out) == 0
        assert out.exists()
        assert out.read_text(encoding="utf-8") == ""
