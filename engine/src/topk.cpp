#include "topk.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <utility>

// ---------------------------------------------------------------------------
// top_k — bounded min-heap selection.
//
// Time:  O(n log k)  —  one heap-push per candidate, heap capped at k.
// Space: O(k)        —  the heap never exceeds k + 1 elements.
// ---------------------------------------------------------------------------

std::vector<Result> top_k(
    const std::unordered_map<int, double>& scores,
    int                                    k,
    const SnippetSource&                   snippets)
{
    if (k <= 0 || scores.empty()) return {};

    // -----------------------------------------------------------------------
    // Min-heap ordered by (score ASC, doc_id DESC).
    //
    // The *smallest* element sits on top so we can cheaply evict losers.
    // Tie-break: higher doc_id is "smaller" so it gets evicted first,
    // matching the final Contract-2 ordering (score DESC, doc_id ASC).
    // -----------------------------------------------------------------------
    using Entry = std::pair<double, int>;  // {score, doc_id}

    auto min_order = [](const Entry& a, const Entry& b) {
        if (a.first != b.first) return a.first > b.first;  // higher score = lower priority
        return a.second < b.second;                          // lower doc_id = lower priority
    };

    std::priority_queue<Entry, std::vector<Entry>, decltype(min_order)> heap(min_order);

    // O(n log k): push each candidate; pop when heap exceeds k.
    for (const auto& [doc_id, score] : scores) {
        heap.push({score, doc_id});
        if (static_cast<int>(heap.size()) > k) {
            heap.pop();  // evict the smallest — O(log k)
        }
    }

    // Drain the heap into a vector.  Heap yields ascending order, so we
    // reverse afterwards to get score DESC (with doc_id ASC for ties).
    // O(k log k)
    std::vector<Result> results;
    results.reserve(heap.size());
    while (!heap.empty()) {
        auto [score, doc_id] = heap.top();
        heap.pop();

        // Build the snippet (first 150 chars of doc text).
        const std::string text = snippets.doc_text(doc_id);
        std::string snippet = text.substr(0, 150);
        if (text.length() > 150) snippet += "...";

        results.push_back({doc_id, score, std::move(snippet)});
    }

    // Heap drained in ascending order; reverse to satisfy Contract 2
    // (score DESC, ties by doc_id ASC).
    std::reverse(results.begin(), results.end());

    return results;
}
