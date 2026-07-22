#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <map>

namespace search {

/// A single search result returned by Engine::search.
struct Result {
    int doc_id;
    double score;
    std::string snippet;
};

/// Metadata stored per document.
struct DocMeta {
    std::string title;
    std::string url;
};

/// Naive in-memory inverted index.
///
/// - build_from_jsonl: parses a JSONL file (Contract 1), whitespace-tokenizes +
///   lowercases each doc's text, and builds a map<term, sorted-unique doc_ids>.
/// - search: tokenizes the query the same way, ANDs the postings lists, and
///   returns up to k Results (score=0.0, snippet=title for now).
class Engine {
public:
    /// Parse a JSONL file and build the in-memory index.
    void build_from_jsonl(const std::string& path);

    /// Search for documents matching ALL query terms (AND semantics).
    /// Returns up to k results sorted by doc_id (score=0.0 for now).
    std::vector<Result> search(const std::string& query, int k) const;

    /// Persist the index to disk. (stub — not yet implemented)
    void save(const std::string& index_dir) const;

    /// Load a persisted index from disk. (stub — not yet implemented)
    void load(const std::string& index_dir);

    // ── Accessors (for testing) ──────────────────────────────────────────

    /// Number of indexed documents.
    size_t doc_count() const { return docs_.size(); }

    /// Number of unique terms in the index.
    size_t term_count() const { return index_.size(); }

    /// Return the postings list for a term, or empty if absent.
    std::vector<int> postings(const std::string& term) const;

private:
    /// Whitespace-tokenize and lowercase a string.
    static std::vector<std::string> tokenize(const std::string& text);

    /// Intersect two sorted vectors of doc_ids.
    static std::vector<int> intersect(const std::vector<int>& a,
                                      const std::vector<int>& b);

    /// term -> sorted unique doc_ids
    std::map<std::string, std::vector<int>> index_;

    /// doc_id -> metadata
    std::unordered_map<int, DocMeta> docs_;
};

}  // namespace search
