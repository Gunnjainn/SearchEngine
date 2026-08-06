# Evaluation

Labeled query sets and result files for measuring search quality (precision,
recall, nDCG, etc.).

## Status

Corpus pinned and reproducible. Query labels and the metrics harness are not
implemented yet.

## The corpus

Everything — benchmarks and quality metrics alike — runs against the single
corpus at `ingest/data/corpus.jsonl`. There is deliberately no separate "eval
corpus": the moment two exist, every number invites "which corpus is that from?"

It is **not committed**. It is reproducible instead:

```bash
python ingest/hn_fetch.py --target 100000 --before 1785000000 \
    -o ingest/data/corpus.jsonl
```

or simply `make corpus`, which runs exactly that in a container.

| | |
|---|---|
| Documents | 100,000 |
| Size | ~30 MB |
| Source | Algolia HN Search API, `tags=story` |
| Pin | `created_at_i < 1785000000` |
| Format | Contract 1 |
| sha256 | `15e3c9c79eafe69e8ce0a54fde77692f6b222d152180674ec47ff951d048e801` |
| Fetched | 2026-08-05, 100 requests, ~9 min |

### Why a recipe rather than a committed file

`search_by_date` returns newest-first, so an unpinned crawl gathers a different
corpus every day — which would make every measurement unreproducible. `--before`
fixes the starting timestamp, and old HN stories do not change, so the same
command returns the same documents. Verified: two independent runs with the same
pin produced **byte-identical** output.

That turns a 30 MB blob that would bloat every clone into four lines of shell.
Record the sha256 alongside your results so drift is detectable — a moderated
story can still disappear from the API:

```bash
sha256sum ingest/data/corpus.jsonl
```

## Still to build

- A labeled query set — roughly 40 queries with relevance judgements
- A harness computing precision@10 and nDCG@10 for a given mode
- Latency at p50/p95. Contract 2's `latency_ms` is an integer; `engine/tools/bench_query.cpp`
  already measures in microseconds and is the better source.

### Label by pooling, not by reading the corpus

100,000 documents cannot be read to find what is relevant to 40 queries — and
labeling against a smaller subset would be actively wrong. On the full corpus the
engine surfaces relevant documents that were never labeled, and each one counts
as a miss, so precision@10 would measure label coverage rather than ranking.

Use pooling, as TREC has since 1992:

1. Run each mode (keyword, then hybrid) over the query set
2. Take the **union of the top 10** from both
3. Judge only that pool

That is at most 40 × 10 × 2 = 800 judgements, and the two modes overlap heavily,
so realistically 400–500.

State the known limitation in the writeup: pooling can only judge what the
systems retrieved, so a document neither surfaces is invisible, which biases
*absolute* precision upward. It does **not** bias the keyword-versus-hybrid
comparison, which is the number that matters — both modes contribute to the pool
on equal terms.
