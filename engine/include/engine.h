#pragma once
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>


#include "bm25.h"        // Posting, PostingsSource, BM25Params, score_docs
#include "doc_store.h"   // DocStore, DocMeta
#include "query.h"       // QueryMode, parse_query, candidate_docs
#include "result.h"      // Result
#include "topk.h"        // SnippetSource, top_k

class Engine : public PostingsSource, public SnippetSource {


public:
    // Builds the index and points the doc store at `path`. The JSONL must stay
    // unchanged until save() is called, because the store addresses documents by
    // their byte offset within it rather than copying the text into memory.
    void build_from_jsonl(const std::string& path);

    // ── Persistence ───────────────────────────────────────────────────────
    // save() writes the index to `dir` (created if needed) and repoints the doc
    // store at the saved copy, so the engine no longer depends on the JSONL.
    // load() rebuilds everything build_from_jsonl produced, so a fresh process
    // can serve queries with no JSONL present. Format: see engine/README.md.
    //
    // Both return false on I/O error, a corrupt file, or a version mismatch,
    // and report the reason on stderr. A failed load() leaves the engine
    // exactly as it was rather than half-populated.
    bool save(const std::string& dir);
    bool load(const std::string& dir);

    // Contract 2 search: parse with the shared tokenizer, generate candidates,
    // rank with BM25, take the top k, then build a snippet around the matched
    // terms of each survivor.
    //
    // Defaults to AND across query terms — a document must contain all of them.
    // The overload takes search::QueryMode::Or for the broader union instead.
    std::vector<Result> search(const std::string& query, int k) const;
    std::vector<Result> search(const std::string& query, int k,
                               search::QueryMode mode) const;

    // ── BM25 scoring ──────────────────────────────────────────────────────

    // Accumulate BM25 contributions for each query term across candidate
    // documents. Returns doc_id -> total BM25 score.
    std::unordered_map<int, double> score_docs(
        const std::vector<std::string>& query_terms) const;

    // ── Postings access (SYNC POINT 2a) ───────────────────────────────────

    // Postings for `term`, ascending by doc_id, with exactly one entry per
    // document that contains it. Unknown terms yield a shared empty list, so
    // the reference is always safe to bind.
    const std::vector<Posting>& postings(const std::string& term) const override;

    // Token count of a document after tokenization; 0 if doc_id is unknown.
    int doc_length(int doc_id) const override;

    int    num_docs() const override;
    double avg_doc_length() const override;  // 0.0 when the index is empty

    // Distinct terms in the dictionary.
    std::size_t num_terms() const;

    // ── Doc store (SYNC POINT 2a) ─────────────────────────────────────────

    // Read from the backing file on demand — the text is not held in memory.
    // Empty string / default-constructed DocMeta if doc_id is unknown.
    std::string doc_text(int doc_id) const override;
    DocMeta     doc_meta(int doc_id) const;

    // Read-only view of the store, for reporting its footprint.
    const search::DocStore& doc_store() const { return docs; }

private:
    // NOTE: text analysis lives in search::tokenize (tokenizer.h). Both
    // build_from_jsonl and search go through it, so index terms and query
    // terms are always produced by the same pipeline.

    // Documents: offsets only, text on disk. See doc_store.h.
    search::DocStore docs;

    // Term dictionary -> postings, each list ascending by doc_id.
    std::unordered_map<std::string, std::vector<Posting>> inverted_index;

    // Sum of every doc_length, so avg_doc_length() cannot go stale.
    long long total_tokens = 0;

    // BM25 parameters (k1=1.2, b=0.75 — configurable via BM25Params).
    BM25Params bm25_params;
};
