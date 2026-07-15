#!/usr/bin/env python3
"""Fetch ~1,000 Hacker News stories via the Algolia HN Search API and emit
Contract-1 JSONL (one JSON object per line).

Usage:
    python hn_fetch.py                         # default: 1000 stories -> data/corpus.sample.jsonl
    python hn_fetch.py --target 500 -o out.jsonl
"""

from __future__ import annotations

import argparse
import json
import logging
import sys
import time
from pathlib import Path
from typing import Any

import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

# ── constants ────────────────────────────────────────────────────────────────
API_URL = "https://hn.algolia.com/api/v1/search_by_date"
HN_ITEM_URL = "https://news.ycombinator.com/item?id={}"
HITS_PER_PAGE = 20  # Algolia default max
MAX_PAGES = 500  # Algolia hard limit

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

def fetch_stories(
    target: int = 1000,
    session: requests.Session | None = None,
    delay: float = 0.25,
) -> list[dict[str, Any]]:
    """Paginate through the Algolia HN API until we collect *target* docs.

    Returns a list of Contract-1 dicts (may be fewer than *target* if the API
    runs out of results or pages).
    """
    if session is None:
        session = build_session()

    docs: list[dict[str, Any]] = []
    seen_ids: set[int] = set()
    page = 0

    while len(docs) < target and page < MAX_PAGES:
        params = {"tags": "story", "page": page, "hitsPerPage": HITS_PER_PAGE}
        logger.info("Fetching page %d (collected %d / %d) ...", page, len(docs), target)

        resp = session.get(API_URL, params=params, timeout=15)
        if resp.status_code != 200:
            logger.warning("Page %d returned HTTP %d — stopping.", page, resp.status_code)
            break

        data = resp.json()
        hits = data.get("hits", [])
        if not hits:
            logger.info("No more hits at page %d — stopping.", page)
            break

        for hit in hits:
            doc = hit_to_doc(hit)
            if doc is None:
                continue
            if doc["doc_id"] in seen_ids:
                continue
            seen_ids.add(doc["doc_id"])
            docs.append(doc)
            if len(docs) >= target:
                break

        page += 1
        # Be polite to the free API
        if page < MAX_PAGES and len(docs) < target:
            time.sleep(delay)

    return docs


# ── I/O ──────────────────────────────────────────────────────────────────────

def write_jsonl(docs: list[dict[str, Any]], path: Path) -> None:
    """Write a list of Contract-1 dicts as JSONL."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        for doc in docs:
            fh.write(json.dumps(doc, ensure_ascii=False) + "\n")
    logger.info("Wrote %d documents to %s", len(docs), path)


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
        default=Path(__file__).resolve().parent / "data" / "corpus.sample.jsonl",
        help="Output JSONL path (default: ingest/data/corpus.sample.jsonl).",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> None:
    args = parse_args(argv)
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
    )
    docs = fetch_stories(target=args.target)
    write_jsonl(docs, args.output)
    print(f"Done: {len(docs)} stories written to {args.output}")


if __name__ == "__main__":
    main()
