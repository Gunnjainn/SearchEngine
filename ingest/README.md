# Ingest

Python ingestion pipeline that converts raw corpus data into the
[Document JSONL](../README.md#contract-1--document-jsonl-one-json-object-per-line) format.

## Scripts

### `hn_fetch.py`

Fetches ~1,000 Hacker News stories via the [Algolia HN Search API](https://hn.algolia.com/api)
and outputs Contract-1 JSONL.

```bash
pip install -r requirements.txt
python hn_fetch.py                          # default: 1000 stories → data/corpus.sample.jsonl
python hn_fetch.py --target 500 -o out.jsonl
```

**Features:**
- Paginates through `search_by_date?tags=story` until target count is reached.
- Automatic retry with exponential backoff (429, 5xx).
- Deduplicates by `doc_id` (HN `objectID`).
- Falls back to `https://news.ycombinator.com/item?id=...` when a story has no external URL.

## Tests

```bash
python -m pytest test_hn_fetch.py -v
```
