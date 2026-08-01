#pragma once
#include <string>
#include <vector>

#include "bm25.h"  // PostingsSource, Posting

namespace search {

// ---------------------------------------------------------------------------
// QueryMode — how candidate documents are chosen before ranking.
//
//   And — a document must contain every distinct query term. The default:
//         precise, and it keeps BM25 off documents that merely share a common
//         word with the query.
//   Or  — a document need contain only one term. Broader recall, and the
//         useful fallback when And returns nothing.
//
// Both modes rank the survivors with the same BM25 scorer; the mode only
// decides which documents get scored at all.
// ---------------------------------------------------------------------------
enum class QueryMode { And, Or };

// ---------------------------------------------------------------------------
// Query — a raw query string after the shared tokenizer has run on it.
// ---------------------------------------------------------------------------
struct Query {
    // Stemmed terms in query order. Repeats are kept: a term written twice is
    // scored twice, which is how BM25 expresses query term frequency.
    std::vector<std::string> terms;
    QueryMode mode = QueryMode::And;
};

// Parse `raw` with search::tokenize — the exact function the index build uses,
// so a query term and the indexed term for the same word are identical.
// A query of only stopwords or punctuation yields no terms.
Query parse_query(const std::string& raw, QueryMode mode = QueryMode::And);

// Documents worth scoring, ascending by doc_id and free of duplicates.
//
// And intersects the postings lists of the distinct terms, smallest first, so
// the work is bounded by the rarest term rather than the corpus. Or unions
// them. Either way the result is the candidate set, not a ranking.
std::vector<int> candidate_docs(const Query& query, const PostingsSource& source);

}  // namespace search
