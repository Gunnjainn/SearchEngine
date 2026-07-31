#include "bm25.h"

#include <cmath>

std::unordered_map<int, double> score_docs(
    const std::vector<std::string>& query_terms,
    const PostingsSource&           source,
    const BM25Params&               params)
{
    std::unordered_map<int, double> scores;

    const double N     = static_cast<double>(source.num_docs());
    const double avgdl = source.avg_doc_length();
    const double k1    = params.k1;
    const double b     = params.b;

    // If there are no documents or every document tokenized to nothing,
    // BM25 length normalisation would divide by zero.
    if (N <= 0.0 || avgdl <= 0.0) return scores;

    for (const auto& term : query_terms) {
        const std::vector<Posting>& plist = source.postings(term);
        if (plist.empty()) continue;

        // The list has exactly one entry per document that contains the
        // term, so its length IS the document frequency.
        const double df  = static_cast<double>(plist.size());
        const double idf = std::log(1.0 + (N - df + 0.5) / (df + 0.5));

        for (const auto& p : plist) {
            const double tf = static_cast<double>(p.term_freq);
            const double dl = static_cast<double>(source.doc_length(p.doc_id));

            const double numerator   = tf * (k1 + 1.0);
            const double denominator = tf + k1 * (1.0 - b + b * (dl / avgdl));

            scores[p.doc_id] += idf * numerator / denominator;
        }
    }

    return scores;
}
