# Evaluation

Labeled query sets and result files for measuring search quality (precision,
recall, nDCG, etc.).

## Status

Corpus frozen. Query labels and the metrics harness are not implemented yet.

## The frozen corpus

`corpus.frozen.jsonl` is the corpus every evaluation runs against.

| | |
|---|---|
| Documents | 968 |
| Size | 296,192 bytes |
| Format | Contract 1, validated: all four keys present, `doc_id` unique |
| sha256 | `8102dae13d7cdc498245de9b8d6b1229a3400d9c5c452033887a456da39beb91` |
| Frozen | 2026-08-05, copied from `ingest/data/corpus.sample.jsonl` |

**Why it is committed rather than regenerated.** `ingest/hn_fetch.py` fetches
*recent* Hacker News stories, so running it again returns a different set of
documents — and `ingest/data/` is gitignored, so nothing preserved it. Every
quality number we care about is a comparison, keyword versus hybrid, and a
comparison across two different document sets measures nothing. Freezing the
corpus is what makes the two arms comparable, and what lets anyone reproduce a
number months from now.

Relevance labels are the other half of that. They are human judgement, the one
input that cannot be regenerated from code, and they are only valid against the
document set they were written for.

Check the copy has not drifted:

```bash
sha256sum eval/corpus.frozen.jsonl
# 8102dae13d7cdc498245de9b8d6b1229a3400d9c5c452033887a456da39beb91
```

Point the engine at it instead of the live corpus:

```bash
JSONL_PATH=eval/corpus.frozen.jsonl INDEX_PATH=./eval-index search_engine
```

## Still to build

- A labeled query set — roughly 40 queries with relevance judgements
- A harness computing precision@10 and nDCG@10 for a given ranking
- Latency at p50/p95. Note Contract 2's `latency_ms` is an **integer** and reports
  0–2 ms on this corpus, too coarse to take percentiles of, so the harness needs
  its own microsecond timing rather than trusting that field.
