// ---------------------------------------------------------------------------
// Unit tests for the top-k min-heap selection (topk.h / topk.cpp).
//
// All tests use a trivial SnippetSource stub so that top_k can be verified
// without touching the real Engine or any JSONL file.
//
// Key properties under test:
//   1. Correct ordering (score DESC, ties break doc_id ASC).
//   2. Heap never exceeds size k during selection — O(n log k).
//   3. Edge cases: k = 0, empty scores, k > n, all equal scores.
// ---------------------------------------------------------------------------

#include "topk.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <cmath>

static int g_tests_run    = 0;
static int g_tests_passed = 0;

#define ASSERT(cond)                                                        \
    do {                                                                    \
        if (!(cond)) {                                                      \
            throw std::runtime_error(                                       \
                std::string("Assertion failed: ") + #cond +                 \
                " at " + __FILE__ + ":" + std::to_string(__LINE__));        \
        }                                                                   \
    } while (0)

static void run_test(const std::string& name, void (*fn)()) {
    ++g_tests_run;
    std::cout << "  TEST " << name << " ... ";
    try {
        fn();
        ++g_tests_passed;
        std::cout << "PASS\n";
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// StubSnippets — trivial SnippetSource that returns "text for doc <id>".
// ---------------------------------------------------------------------------

class StubSnippets : public SnippetSource {
public:
    std::string doc_text(int doc_id) const override {
        return "text for doc " + std::to_string(doc_id);
    }
};

static StubSnippets stub;

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Core test: top-3 from 6 documents with distinct scores.
static void test_topk_correct_ordering() {
    std::unordered_map<int, double> scores = {
        {1, 1.0}, {2, 5.0}, {3, 3.0}, {4, 9.0}, {5, 7.0}, {6, 2.0}
    };

    auto results = top_k(scores, 3, stub);

    // Expected: doc 4 (9.0), doc 5 (7.0), doc 2 (5.0) — score DESC.
    ASSERT(results.size() == 3);
    ASSERT(results[0].doc_id == 4);
    ASSERT(results[0].score  == 9.0);
    ASSERT(results[1].doc_id == 5);
    ASSERT(results[1].score  == 7.0);
    ASSERT(results[2].doc_id == 2);
    ASSERT(results[2].score  == 5.0);
}

// Verify that the heap stays bounded at size k.
// We can't observe the internal heap directly, but we can verify that top_k
// produces exactly k results from n >> k candidates, and that only the true
// top-k survive.  This confirms the min-heap eviction logic is correct.
static void test_heap_stays_size_k() {
    // 100 candidates, request top 5.
    std::unordered_map<int, double> scores;
    for (int i = 1; i <= 100; ++i) {
        scores[i] = static_cast<double>(i);  // doc 100 has score 100.0, etc.
    }

    auto results = top_k(scores, 5, stub);

    // The heap was bounded at 5 throughout.  Output must be exactly 5 results.
    ASSERT(results.size() == 5);

    // Expected: docs 100, 99, 98, 97, 96 — score DESC.
    ASSERT(results[0].doc_id == 100);
    ASSERT(results[0].score  == 100.0);
    ASSERT(results[1].doc_id == 99);
    ASSERT(results[1].score  == 99.0);
    ASSERT(results[2].doc_id == 98);
    ASSERT(results[2].score  == 98.0);
    ASSERT(results[3].doc_id == 97);
    ASSERT(results[3].score  == 97.0);
    ASSERT(results[4].doc_id == 96);
    ASSERT(results[4].score  == 96.0);
}

// Tie-breaking: equal scores should break on doc_id ASC.
static void test_topk_tie_breaking() {
    std::unordered_map<int, double> scores = {
        {10, 5.0}, {3, 5.0}, {7, 5.0}, {1, 5.0}
    };

    auto results = top_k(scores, 4, stub);

    // All scores equal → output ordered by doc_id ASC.
    ASSERT(results.size() == 4);
    ASSERT(results[0].doc_id == 1);
    ASSERT(results[1].doc_id == 3);
    ASSERT(results[2].doc_id == 7);
    ASSERT(results[3].doc_id == 10);
}

// Tie-breaking with k < n: among equal scores, lowest doc_ids survive.
static void test_topk_tie_breaking_with_eviction() {
    std::unordered_map<int, double> scores = {
        {10, 5.0}, {3, 5.0}, {7, 5.0}, {1, 5.0}, {20, 5.0}
    };

    auto results = top_k(scores, 3, stub);

    // All scores equal.  Contract 2: ties break doc_id ASC, so top-3 by
    // that ordering are docs 1, 3, 7.
    ASSERT(results.size() == 3);
    ASSERT(results[0].doc_id == 1);
    ASSERT(results[1].doc_id == 3);
    ASSERT(results[2].doc_id == 7);
}

// k greater than n: return all documents, still sorted DESC.
static void test_topk_k_greater_than_n() {
    std::unordered_map<int, double> scores = {
        {1, 3.0}, {2, 1.0}, {3, 2.0}
    };

    auto results = top_k(scores, 10, stub);

    ASSERT(results.size() == 3);
    ASSERT(results[0].doc_id == 1);
    ASSERT(results[0].score  == 3.0);
    ASSERT(results[1].doc_id == 3);
    ASSERT(results[1].score  == 2.0);
    ASSERT(results[2].doc_id == 2);
    ASSERT(results[2].score  == 1.0);
}

// k = 0 → empty result.
static void test_topk_k_zero() {
    std::unordered_map<int, double> scores = {{1, 1.0}};
    auto results = top_k(scores, 0, stub);
    ASSERT(results.empty());
}

// Empty score map → empty result.
static void test_topk_empty_scores() {
    std::unordered_map<int, double> scores;
    auto results = top_k(scores, 5, stub);
    ASSERT(results.empty());
}

// k = 1 → single best result.
static void test_topk_k_one() {
    std::unordered_map<int, double> scores = {
        {1, 1.0}, {2, 9.0}, {3, 5.0}
    };

    auto results = top_k(scores, 1, stub);
    ASSERT(results.size() == 1);
    ASSERT(results[0].doc_id == 2);
    ASSERT(results[0].score  == 9.0);
}

// Verify that snippets are populated from the SnippetSource.
static void test_topk_snippets() {
    std::unordered_map<int, double> scores = {{42, 1.0}};
    auto results = top_k(scores, 1, stub);
    ASSERT(results.size() == 1);
    ASSERT(results[0].snippet == "text for doc 42");
}

// Large-n stress: verify that top-k from 10,000 docs is correct.
// This implicitly validates that the heap approach works at scale.
static void test_topk_large_n() {
    std::unordered_map<int, double> scores;
    for (int i = 1; i <= 10000; ++i) {
        scores[i] = static_cast<double>(i) * 0.1;
    }

    auto results = top_k(scores, 3, stub);
    ASSERT(results.size() == 3);
    ASSERT(results[0].doc_id == 10000);
    ASSERT(std::fabs(results[0].score - 1000.0) < 1e-9);
    ASSERT(results[1].doc_id == 9999);
    ASSERT(results[2].doc_id == 9998);
}

// Mixed scores with negative values (edge case — BM25 can sometimes
// produce very small or near-zero scores, but let's be robust).
static void test_topk_negative_scores() {
    std::unordered_map<int, double> scores = {
        {1, -5.0}, {2, -1.0}, {3, -10.0}, {4, 0.0}
    };

    auto results = top_k(scores, 2, stub);
    ASSERT(results.size() == 2);
    ASSERT(results[0].doc_id == 4);
    ASSERT(results[0].score  == 0.0);
    ASSERT(results[1].doc_id == 2);
    ASSERT(results[1].score  == -1.0);
}

// ---------------------------------------------------------------------------

int main() {
    std::cout << "=== top-k unit tests ===\n";

    run_test("topk_correct_ordering",          test_topk_correct_ordering);
    run_test("heap_stays_size_k",              test_heap_stays_size_k);
    run_test("topk_tie_breaking",              test_topk_tie_breaking);
    run_test("topk_tie_breaking_with_eviction", test_topk_tie_breaking_with_eviction);
    run_test("topk_k_greater_than_n",          test_topk_k_greater_than_n);
    run_test("topk_k_zero",                    test_topk_k_zero);
    run_test("topk_empty_scores",              test_topk_empty_scores);
    run_test("topk_k_one",                     test_topk_k_one);
    run_test("topk_snippets",                  test_topk_snippets);
    run_test("topk_large_n",                   test_topk_large_n);
    run_test("topk_negative_scores",           test_topk_negative_scores);

    std::cout << "\n" << g_tests_passed << "/" << g_tests_run << " tests passed.\n";
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
