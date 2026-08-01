#include "query.h"

#include <algorithm>

#include "tokenizer.h"

namespace search {
namespace {

// Distinct terms, sorted. Candidate generation cares only about which terms
// appear, not how often the user typed them.
std::vector<std::string> distinct_terms(const std::vector<std::string>& terms) {
    std::vector<std::string> out = terms;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Intersect two ascending doc_id lists in one pass.
std::vector<int> intersect(const std::vector<int>& a, const std::vector<int>& b) {
    std::vector<int> out;
    out.reserve(std::min(a.size(), b.size()));

    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            out.push_back(a[i]);
            ++i;
            ++j;
        } else if (a[i] < b[j]) {
            ++i;
        } else {
            ++j;
        }
    }
    return out;
}

}  // namespace

Query parse_query(const std::string& raw, QueryMode mode) {
    Query query;
    query.terms = tokenize(raw);
    query.mode  = mode;
    return query;
}

std::vector<int> candidate_docs(const Query& query, const PostingsSource& source) {
    const std::vector<std::string> terms = distinct_terms(query.terms);
    if (terms.empty()) return {};

    // Collect the postings lists once; postings() returns a reference, so this
    // is a list of pointers rather than a copy of the index.
    std::vector<const std::vector<Posting>*> lists;
    lists.reserve(terms.size());
    for (const std::string& term : terms) {
        const std::vector<Posting>& plist = source.postings(term);
        // Under And a missing term ends it: no document can contain them all.
        if (plist.empty() && query.mode == QueryMode::And) return {};
        if (!plist.empty()) lists.push_back(&plist);
    }
    if (lists.empty()) return {};

    if (query.mode == QueryMode::Or) {
        std::vector<int> out;
        for (const std::vector<Posting>* plist : lists) {
            for (const Posting& p : *plist) out.push_back(p.doc_id);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // And: start from the rarest term so the working set is as small as
    // possible from the first step onward.
    std::sort(lists.begin(), lists.end(),
              [](const std::vector<Posting>* a, const std::vector<Posting>* b) {
                  return a->size() < b->size();
              });

    std::vector<int> result;
    result.reserve(lists.front()->size());
    for (const Posting& p : *lists.front()) result.push_back(p.doc_id);

    for (std::size_t i = 1; i < lists.size() && !result.empty(); ++i) {
        std::vector<int> other;
        other.reserve(lists[i]->size());
        for (const Posting& p : *lists[i]) other.push_back(p.doc_id);
        result = intersect(result, other);
    }

    return result;
}

}  // namespace search
