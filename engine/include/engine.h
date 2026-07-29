#pragma once
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

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

// Title and URL of an indexed document (SYNC POINT 2a).
struct DocMeta {
    std::string title;
    std::string url;
};

// ---------------------------------------------------------------------------
// Document — a stored document plus its indexing statistics.
// ---------------------------------------------------------------------------
struct Document {
    int         id;
    std::string title;
    std::string url;
    std::string text;
    int         length;  // token count after search::tokenize
};

class Engine {
public:
    void load(const std::string& path);
    void build_from_jsonl(const std::string& path);
    void save(const std::string& path);
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

    // Empty string / default-constructed DocMeta if doc_id is unknown.
    std::string doc_text(int doc_id) const;
    DocMeta     doc_meta(int doc_id) const;

private:
    // NOTE: text analysis lives in search::tokenize (tokenizer.h). Both
    // build_from_jsonl and search go through it, so index terms and query
    // terms are always produced by the same pipeline.

    // nullptr if doc_id was never indexed.
    const Document* find_doc(int doc_id) const;

    std::vector<Document>                docs;       // in insertion order
    std::unordered_map<int, std::size_t> doc_index;  // doc_id -> index into docs

    // Term dictionary -> postings, each list ascending by doc_id.
    std::unordered_map<std::string, std::vector<Posting>> inverted_index;

    // Sum of every doc_length, so avg_doc_length() cannot go stale.
    long long total_tokens = 0;

    // BM25 parameters
    const double k1 = 1.5;
    const double b = 0.75;
};
