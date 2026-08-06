#!/usr/bin/env python3
"""Fetch Hacker News stories via the Algolia HN Search API and emit Contract-1
JSONL (one JSON object per line).

Algolia caps a single query at 1000 results, so this walks backwards through HN
by timestamp instead of paging. Pin --before to make a crawl reproducible.

Usage:
    python hn_fetch.py --target 100000 --before 1785000000   # the project corpus
    python hn_fetch.py --target 500 -o out.jsonl
"""

from __future__ import annotations

import argparse
import json
import logging
import sys
import time
from collections.abc import Iterable, Iterator
from pathlib import Path
from typing import Any

import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

# ── constants ────────────────────────────────────────────────────────────────
API_URL = "https://hn.algolia.com/api/v1/search_by_date"
HN_ITEM_URL = "https://news.ycombinator.com/item?id={}"

# Algolia serves up to 1000 hits in one request.
HITS_PER_PAGE = 1000

# ...but it also caps *any single query* at 1000 results total: ask for
# hitsPerPage=1000 and the response comes back with nbPages=1, and page=1 is
# empty. Paging alone therefore cannot exceed 1000 documents, which is why the
# original sample corpus stalled at ~968.
#
# The way past it is to change the query rather than the page: filter on
# created_at_i < (oldest timestamp seen so far) and ask for page 0 again. Each
# request opens a fresh 1000-result window further back in HN's history, and the
# windows do not overlap. Verified against the live API: three windows returned
# 3000 distinct documents with zero duplicates.
MAX_REQUESTS = 5000  # safety valve, ~5M documents at 1000 per request

logger = logging.getLogger(__name__)


# ── networking ───────────────────────────────────────────────────────────────

def build_session(
    retries: int = 5,
    backoff_factor: float = 0.5,
    status_forcelist: tuple[int, ...] = (429, 500, 502, 503, 504),
) -> requests.Session:
    """Return a requests.Session with automatic retry/backoff."""
    session = requests.Session()
    retry = Retry(
        total=retries,
        backoff_factor=backoff_factor,
        status_forcelist=list(status_forcelist),
        allowed_methods=["GET"],
        raise_on_status=False,
    )
    adapter = HTTPAdapter(max_retries=retry)
    session.mount("https://", adapter)
    session.mount("http://", adapter)
    return session


# ── mapping ──────────────────────────────────────────────────────────────────

def hit_to_doc(hit: dict[str, Any]) -> dict[str, Any] | None:
    """Convert an Algolia hit to a Contract-1 document.

    Returns None when the hit is unusable (missing both title and text).
    """
    object_id = hit.get("objectID")
    if object_id is None:
        return None

    try:
        doc_id = int(object_id)
    except (ValueError, TypeError):
        return None

    title = (hit.get("title") or "").strip()

    # Build the text body: title + any available body content
    story_text = (hit.get("story_text") or "").strip()
    comment_text = (hit.get("comment_text") or "").strip()
    body = story_text or comment_text
    text = f"{title}\n{body}".strip() if body else title

    if not text:
        return None

    url = (hit.get("url") or "").strip()
    if not url:
        url = HN_ITEM_URL.format(doc_id)

    return {
        "doc_id": doc_id,
        "title": title,
        "url": url,
        "text": text,
    }


# ── fetching ─────────────────────────────────────────────────────────────────

def iter_stories(
    target: int = 1000,
    session: requests.Session | None = None,
    delay: float = 0.25,
    hits_per_page: int = HITS_PER_PAGE,
    before: int | None = None,
) -> Iterator[dict[str, Any]]:
    """Yield Contract-1 documents, walking backwards through HN by timestamp.

    A generator rather than a list so a large crawl can be streamed straight to
    disk instead of being accumulated in memory.

    Each request asks for page 0 of a window older than everything seen so far;
    see the MAX_REQUESTS comment for why paging cannot be used instead.

    `before` pins the starting point to a Unix timestamp. Without it the walk
    starts at "now", so the same command run tomorrow returns a different corpus
    — which makes any measurement taken against it unreproducible. With it, the
    same command returns the same documents, because old stories do not change.
    """
    if session is None:
        session = build_session()

    seen_ids: set[int] = set()
    cursor: int | None = before  # created_at_i upper bound, exclusive
    yielded = 0
    requests_made = 0

    while yielded < target and requests_made < MAX_REQUESTS:
        params: dict[str, Any] = {
            "tags": "story",
            "page": 0,
            "hitsPerPage": min(hits_per_page, target - yielded) if target < hits_per_page
                           else hits_per_page,
        }
        if cursor is not None:
            params["numericFilters"] = f"created_at_i<{cursor}"

        resp = session.get(API_URL, params=params, timeout=30)
        requests_made += 1
        if resp.status_code != 200:
            logger.warning(
                "Request %d returned HTTP %d — stopping with %d documents.",
                requests_made, resp.status_code, yielded,
            )
            break

        hits = resp.json().get("hits", [])
        if not hits:
            logger.info("No more hits after %d documents — stopping.", yielded)
            break

        new_in_window = 0
        for hit in hits:
            doc = hit_to_doc(hit)
            if doc is None or doc["doc_id"] in seen_ids:
                continue
            seen_ids.add(doc["doc_id"])
            new_in_window += 1
            yielded += 1
            yield doc
            if yielded >= target:
                break

        if yielded >= target:
            break

        # Advance the window to just before the oldest story in this batch.
        timestamps = [h["created_at_i"] for h in hits if h.get("created_at_i")]
        next_cursor = min(timestamps) if timestamps else None

        # Three ways to make no progress, each of which would otherwise spin
        # forever: nothing new in the window, no timestamp to move to, or a
        # cursor that did not actually move.
        if new_in_window == 0:
            logger.info("Window produced no new documents — stopping at %d.", yielded)
            break
        if next_cursor is None:
            logger.info("No created_at_i to page on — stopping at %d.", yielded)
            break
        if cursor is not None and next_cursor >= cursor:
            logger.info("Cursor stopped advancing — stopping at %d.", yielded)
            break

        cursor = next_cursor
        if requests_made % 10 == 0:
            logger.info("%d / %d documents (%d requests)", yielded, target, requests_made)

        time.sleep(delay)  # be polite to the free API

    logger.info("Collected %d documents in %d requests.", yielded, requests_made)


def fetch_stories(
    target: int = 1000,
    session: requests.Session | None = None,
    delay: float = 0.25,
) -> list[dict[str, Any]]:
    """Collect *target* documents into a list.

    Kept for callers and tests that want everything in memory. Prefer
    iter_stories for large crawls.
    """
    return list(iter_stories(target=target, session=session, delay=delay))


# ── I/O ──────────────────────────────────────────────────────────────────────

def write_jsonl(docs: list[dict[str, Any]], path: Path) -> None:
    """Write a list of Contract-1 dicts as JSONL."""
    written = write_jsonl_stream(docs, path)
    logger.info("Wrote %d documents to %s", written, path)


def write_jsonl_stream(docs: Iterable[dict[str, Any]], path: Path) -> int:
    """Write documents as JSONL as they arrive; return how many were written.

    Streaming matters at scale: a 100k-document crawl never has to hold the
    corpus in memory, and a run interrupted partway leaves a usable file rather
    than nothing.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with open(path, "w", encoding="utf-8") as fh:
        for doc in docs:
            fh.write(json.dumps(doc, ensure_ascii=False) + "\n")
            count += 1
    return count


# ── CLI ──────────────────────────────────────────────────────────────────────

def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Fetch Hacker News stories into Contract-1 JSONL."
    )
    parser.add_argument(
        "--target",
        type=int,
        default=1000,
        help="Number of stories to fetch (default: 1000).",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent / "data" / "corpus.jsonl",
        help="Output JSONL path (default: ingest/data/corpus.jsonl).",
    )
    parser.add_argument(
        "--hits-per-page",
        type=int,
        default=HITS_PER_PAGE,
        help=f"Documents per API request, max 1000 (default: {HITS_PER_PAGE}).",
    )
    parser.add_argument(
        "--delay",
        type=float,
        default=0.25,
        help="Seconds to wait between requests (default: 0.25).",
    )
    parser.add_argument(
        "--before",
        type=int,
        default=None,
        metavar="UNIX_TS",
        help=(
            "Start from stories older than this Unix timestamp. Pin it to make "
            "the crawl reproducible: without it the walk starts at 'now', so "
            "the same command returns a different corpus every day."
        ),
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> None:
    args = parse_args(argv)
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
    )

    started = time.monotonic()
    written = write_jsonl_stream(
        iter_stories(
            target=args.target,
            delay=args.delay,
            hits_per_page=min(args.hits_per_page, HITS_PER_PAGE),
            before=args.before,
        ),
        args.output,
    )
    elapsed = time.monotonic() - started

    size = args.output.stat().st_size if args.output.exists() else 0
    print(
        f"Done: {written} stories -> {args.output} "
        f"({size:,} bytes) in {elapsed:.1f}s"
    )
    if written < args.target:
        print(
            f"Note: asked for {args.target}, got {written}. "
            "The API stopped early — see the log above for the reason."
        )


if __name__ == "__main__":
    main()
