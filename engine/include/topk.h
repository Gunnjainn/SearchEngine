#pragma once
#include <queue>
#include <unordered_map>
#include <vector>

#include "result.h"  // Result

// ---------------------------------------------------------------------------
// top_k — select the k highest-scoring documents using a bounded min-heap.
//
// Given a score map (doc_id -> BM25 score) and a lookup for doc text, returns
// the top k results sorted by score DESC, with ties broken by doc_id ASC.
//
// Time complexity:
//   O(n log k) where n = scores.size(), k = requested result count.
//   We push each candidate onto a min-heap of capacity k.  When the heap
//   exceeds k elements we pop the smallest, so every push/pop pair is
//   O(log k).  After the scan, we drain the heap (O(k log k)) and reverse
//   to get DESC order.
//
// Space complexity:
//   O(k) for the heap; the output vector is also O(k).
//
// Why a min-heap?
//   A min-heap of size k keeps the *smallest* of the current top-k on top.
//   Any new candidate that beats the heap-top replaces it.  This avoids
//   sorting all n candidates (O(n log n)) when k << n.
// ---------------------------------------------------------------------------

struct SnippetSource {
    virtual ~SnippetSource() = default;
    virtual std::string doc_text(int doc_id) const = 0;
};

std::vector<Result> top_k(
    const std::unordered_map<int, double>& scores,
    int                                    k,
    const SnippetSource&                   snippets);

// ---------------------------------------------------------------------------
// top_k — selection only, leaving every Result's snippet empty.
//
// Same ordering and complexity as above. Use this when the snippet depends on
// something top-k has no business knowing, such as the query terms: the caller
// fills the snippets afterwards, and document text is read only for the k
// documents that survived rather than for every candidate.
// ---------------------------------------------------------------------------
std::vector<Result> top_k(
    const std::unordered_map<int, double>& scores,
    int                                    k);
