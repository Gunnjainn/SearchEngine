#pragma once
#include <string>
#include <unordered_map>
#include <vector>

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

// ---------------------------------------------------------------------------
// BM25 configurable constants.
// ---------------------------------------------------------------------------
struct BM25Params {
    double k1 = 1.2;
    double b  = 0.75;
};

// ---------------------------------------------------------------------------
// PostingsSource — the abstract interface BM25 reads from (SYNC POINT 2a).
//
// This mirrors the postings-access surface of Engine so that the scorer can
// be tested against a trivial in-memory stub without touching the real index.
// ---------------------------------------------------------------------------
struct PostingsSource {
    virtual ~PostingsSource() = default;

    virtual const std::vector<Posting>& postings(const std::string& term) const = 0;
    virtual int    doc_length(int doc_id) const = 0;
    virtual int    num_docs() const = 0;
    virtual double avg_doc_length() const = 0;
};

// ---------------------------------------------------------------------------
// score_docs — accumulate BM25 contributions for each query term.
//
// Returns a map from doc_id to total BM25 score.  Only documents that appear
// in at least one postings list have entries.
//
// IDF formula:  idf = ln(1 + (N - df + 0.5) / (df + 0.5))
// TF saturation: idf * (tf * (k1 + 1)) / (tf + k1 * (1 - b + b * dl/avgdl))
// ---------------------------------------------------------------------------
std::unordered_map<int, double> score_docs(
    const std::vector<std::string>& query_terms,
    const PostingsSource&           source,
    const BM25Params&               params = BM25Params{});

