#include "bm25.h"

#include <algorithm>
#include <cmath>

namespace {

// Shared implementation. `candidates` is null to score every document holding
// a query term, or an ascending doc_id list to restrict scoring to it.
std::unordered_map<int, double> score_impl(
    const std::vector<std::string>& query_terms,
    const PostingsSource&           source,
    const BM25Params&               params,
    const std::vector<int>*         candidates)
{
    std::unordered_map<int, double> scores;

    const double N     = static_cast<double>(source.num_docs());
    const double avgdl = source.avg_doc_length();
    const double k1    = params.k1;
    const double b     = params.b;

    // If there are no documents or every document tokenized to nothing,
    // BM25 length normalisation would divide by zero.
    if (N <= 0.0 || avgdl <= 0.0) return scores;
    if (candidates != nullptr && candidates->empty()) return scores;

    for (const auto& term : query_terms) {
        const std::vector<Posting>& plist = source.postings(term);
        if (plist.empty()) continue;

        // The list has exactly one entry per document that contains the
        // term, so its length IS the document frequency.
        //
        // This is the collection-wide document frequency, taken before any
        // candidate restriction: narrowing the candidate set must not change
        // what a term is worth.
        const double df  = static_cast<double>(plist.size());
        const double idf = std::log(1.0 + (N - df + 0.5) / (df + 0.5));

        for (const auto& p : plist) {
            if (candidates != nullptr
                && !std::binary_search(candidates->begin(), candidates->end(), p.doc_id)) {
                continue;
            }

            const double tf = static_cast<double>(p.term_freq);
            const double dl = static_cast<double>(source.doc_length(p.doc_id));

            const double numerator   = tf * (k1 + 1.0);
            const double denominator = tf + k1 * (1.0 - b + b * (dl / avgdl));

            scores[p.doc_id] += idf * numerator / denominator;
        }
    }

    return scores;
}

}  // namespace

std::unordered_map<int, double> score_docs(
    const std::vector<std::string>& query_terms,
    const PostingsSource&           source,
    const BM25Params&               params)
{
    return score_impl(query_terms, source, params, nullptr);
}

std::unordered_map<int, double> score_docs(
    const std::vector<std::string>& query_terms,
    const PostingsSource&           source,
    const BM25Params&               params,
    const std::vector<int>&         candidates)
{
    return score_impl(query_terms, source, params, &candidates);
}
