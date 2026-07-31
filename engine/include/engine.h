#pragma once
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "doc_store.h"  // DocStore, DocMeta

// ---------------------------------------------------------------------------
// Result — a single search hit (Contract 2 element).
// ---------------------------------------------------------------------------
struct Result {
    int         doc_id;
    double      score;
    std::string snippet;
};

// ---------------------------------------------------------------------------
// Posting — one entry in a term's postings list (SYNC POINT 2a).
//
// doc_id is the *external* id from Contract 1, not an internal array index,
// so postings lists are meaningful to callers outside the Engine.
// ---------------------------------------------------------------------------
struct Posting {
    int doc_id;
    int term_freq;
};

class Engine {
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

    std::vector<Result> search(const std::string& query, int k) const;

    // ── Postings access (SYNC POINT 2a) ───────────────────────────────────

    // Postings for `term`, ascending by doc_id, with exactly one entry per
    // document that contains it. Unknown terms yield a shared empty list, so
    // the reference is always safe to bind.
    const std::vector<Posting>& postings(const std::string& term) const;

    // Token count of a document after tokenization; 0 if doc_id is unknown.
    int doc_length(int doc_id) const;

    int    num_docs() const;
    double avg_doc_length() const;  // 0.0 when the index is empty

    // Distinct terms in the dictionary.
    std::size_t num_terms() const;

    // ── Doc store (SYNC POINT 2a) ─────────────────────────────────────────

    // Read from the backing file on demand — the text is not held in memory.
    // Empty string / default-constructed DocMeta if doc_id is unknown.
    std::string doc_text(int doc_id) const;
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

    // BM25 parameters
    const double k1 = 1.5;
    const double b = 0.75;
};
