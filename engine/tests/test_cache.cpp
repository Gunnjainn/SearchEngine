// ---------------------------------------------------------------------------
// Unit tests for the LRU query cache.
//
// The two that matter most:
//   repeated_query_hits_the_cache — the acceptance criterion
//   rebuild_invalidates_the_cache — the correctness trap. A cache that outlives
//       its index serves results for documents that are no longer there, and
//       that failure is silent.
// ---------------------------------------------------------------------------

#include "engine.h"
#include "query_cache.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static int g_tests_run    = 0;
static int g_tests_passed = 0;

#define ASSERT(cond)                                                       \
    do {                                                                   \
        if (!(cond)) {                                                     \
            throw std::runtime_error(                                      \
                std::string("Assertion failed: ") + #cond +                \
                " at " + __FILE__ + ":" + std::to_string(__LINE__));       \
        }                                                                  \
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

static std::vector<Result> made(int doc_id) {
    return {Result{doc_id, 1.0, "snippet " + std::to_string(doc_id)}};
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

static void test_key_ignores_query_wording() {
    using search::QueryMode;
    // Same terms, different surface form -> one entry.
    const auto a = search::cache_key(QueryMode::And, {"run", "compil"}, 10);
    const auto b = search::cache_key(QueryMode::And, {"compil", "run"}, 10);
    ASSERT(a == b);  // order does not affect the result, so it must not split the key
}

static void test_key_separates_what_changes_the_result() {
    using search::QueryMode;
    const auto base = search::cache_key(QueryMode::And, {"run", "compil"}, 10);

    ASSERT(search::cache_key(QueryMode::And, {"run", "compil"}, 5) != base);   // k
    ASSERT(search::cache_key(QueryMode::Or,  {"run", "compil"}, 10) != base);  // mode
    ASSERT(search::cache_key(QueryMode::And, {"run"}, 10) != base);            // terms
    // A repeated term is scored twice, so it is a different query.
    ASSERT(search::cache_key(QueryMode::And, {"run", "run"}, 10) != base);
}

static void test_key_cannot_be_forged_by_term_content() {
    using search::QueryMode;
    // Terms are joined with a separator the tokenizer can never emit, so no
    // pair of term lists can collide by concatenation.
    ASSERT(search::cache_key(QueryMode::And, {"ab", "c"}, 1)
        != search::cache_key(QueryMode::And, {"a", "bc"}, 1));
}

// ---------------------------------------------------------------------------
// Cache mechanics
// ---------------------------------------------------------------------------

static void test_miss_then_hit() {
    search::QueryCache cache(4);
    std::vector<Result> out;

    ASSERT(!cache.get("k1", out));
    ASSERT(cache.stats().misses == 1);
    ASSERT(cache.stats().hits == 0);

    cache.put("k1", made(7));
    ASSERT(cache.get("k1", out));
    ASSERT(out.size() == 1);
    ASSERT(out[0].doc_id == 7);
    ASSERT(out[0].snippet == "snippet 7");

    const auto s = cache.stats();
    ASSERT(s.hits == 1);
    ASSERT(s.misses == 1);
    ASSERT(s.size == 1);
    ASSERT(s.capacity == 4);
}

static void test_hit_rate() {
    search::QueryCache cache(4);
    std::vector<Result> out;
    ASSERT(cache.stats().hit_rate() == 0.0);  // no lookups yet, not a division by zero

    cache.put("k", made(1));
    cache.get("k", out);      // hit
    cache.get("other", out);  // miss
    ASSERT(cache.stats().hit_rate() == 0.5);
}

static void test_eviction_is_bounded_by_capacity() {
    search::QueryCache cache(2);
    std::vector<Result> out;

    cache.put("a", made(1));
    cache.put("b", made(2));
    cache.put("c", made(3));  // evicts "a", the least recently used

    ASSERT(cache.stats().size == 2);
    ASSERT(cache.stats().evictions == 1);
    ASSERT(!cache.get("a", out));
    ASSERT(cache.get("b", out));
    ASSERT(cache.get("c", out));
}

static void test_eviction_follows_recency_not_insertion() {
    search::QueryCache cache(2);
    std::vector<Result> out;

    cache.put("a", made(1));
    cache.put("b", made(2));
    ASSERT(cache.get("a", out));  // "a" is now the most recently used
    cache.put("c", made(3));      // so "b" must be the one to go

    ASSERT(cache.get("a", out));
    ASSERT(!cache.get("b", out));
    ASSERT(cache.get("c", out));
}

static void test_put_replaces_without_growing() {
    search::QueryCache cache(2);
    std::vector<Result> out;

    cache.put("a", made(1));
    cache.put("a", made(99));

    ASSERT(cache.stats().size == 1);
    ASSERT(cache.stats().evictions == 0);
    ASSERT(cache.get("a", out));
    ASSERT(out[0].doc_id == 99);
}

static void test_clear_empties_but_keeps_counters() {
    search::QueryCache cache(4);
    std::vector<Result> out;

    cache.put("a", made(1));
    cache.get("a", out);   // hit 1
    cache.get("zz", out);  // miss 1
    cache.clear();

    const auto s = cache.stats();
    ASSERT(s.size == 0);
    ASSERT(!cache.get("a", out));  // miss 2 — cleared, so the entry is gone
    // Lifetime telemetry survives the clear: 1 hit, 2 misses.
    ASSERT(s.hits == 1);
    ASSERT(cache.stats().hits == 1);
    ASSERT(cache.stats().misses == 2);
}

static void test_capacity_zero_disables_caching() {
    search::QueryCache cache(0);
    std::vector<Result> out;

    cache.put("a", made(1));
    ASSERT(cache.stats().size == 0);
    ASSERT(!cache.get("a", out));
}

static void test_shrinking_capacity_evicts_immediately() {
    search::QueryCache cache(4);
    std::vector<Result> out;
    for (int i = 0; i < 4; ++i) cache.put("k" + std::to_string(i), made(i));
    ASSERT(cache.stats().size == 4);

    cache.set_capacity(2);
    ASSERT(cache.stats().size == 2);

    // Disabling must drop what is held, not leave it readable.
    cache.set_capacity(0);
    ASSERT(cache.stats().size == 0);
    ASSERT(!cache.get("k3", out));
}

// ---------------------------------------------------------------------------
// Thread safety
// ---------------------------------------------------------------------------

// File scope rather than inside the test: MSVC will not let a lambda use a
// function-scope constexpr without capturing it, and capturing constants is noise.
static constexpr int kThreads = 8;
static constexpr int kOpsEach = 2000;

static void test_concurrent_access_is_consistent() {
    search::QueryCache cache(64);

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&cache, t]() {
            std::vector<Result> out;
            for (int i = 0; i < kOpsEach; ++i) {
                const std::string key = "k" + std::to_string((t * 7 + i) % 128);
                if (!cache.get(key, out)) {
                    cache.put(key, made(i));
                } else {
                    // Whatever came back must be a whole, well-formed entry —
                    // never a half-written one.
                    if (out.size() != 1 || out[0].snippet.rfind("snippet ", 0) != 0) {
                        throw std::runtime_error("torn entry read from the cache");
                    }
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    const auto s = cache.stats();
    // Every operation was counted exactly once, and nothing exceeded the bound.
    ASSERT(s.hits + s.misses == static_cast<std::uint64_t>(kThreads) * kOpsEach);
    ASSERT(s.size <= 64);
    ASSERT(s.hits > 0);  // with 128 keys over 16000 ops, hits are certain
}

// ---------------------------------------------------------------------------
// Engine integration
// ---------------------------------------------------------------------------

static std::string write_corpus(const std::string& path, const char* word) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) throw std::runtime_error("cannot write " + path);
    for (int i = 1; i <= 5; ++i) {
        out << R"({"doc_id": )" << i << R"(, "title": "Doc )" << i
            << R"(", "url": "u)" << i << R"(", "text": "the )" << word
            << " appears here in document " << i << R"("})" << "\n";
    }
    out.close();
    return path;
}

// The acceptance criterion.
static void test_repeated_query_hits_the_cache() {
    Engine e;
    e.build_from_jsonl(write_corpus("test_cache_a.jsonl", "compiler"));

    const auto first = e.search("compiler", 5);
    ASSERT(!first.empty());
    ASSERT(e.cache_stats().misses == 1);
    ASSERT(e.cache_stats().hits == 0);

    const auto second = e.search("compiler", 5);
    ASSERT(e.cache_stats().hits == 1);

    // A hit must be indistinguishable from a miss in what it returns.
    ASSERT(second.size() == first.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        ASSERT(first[i].doc_id == second[i].doc_id);
        ASSERT(first[i].score == second[i].score);
        ASSERT(first[i].snippet == second[i].snippet);
    }
}

static void test_equivalent_queries_share_an_entry() {
    Engine e;
    e.build_from_jsonl(write_corpus("test_cache_b.jsonl", "compiler"));

    e.search("compiler", 5);            // miss
    e.search("COMPILERS!", 5);          // same terms after tokenizing -> hit
    e.search("the compiler", 5);        // "the" is a stopword -> hit

    const auto s = e.cache_stats();
    ASSERT(s.misses == 1);
    ASSERT(s.hits == 2);
    ASSERT(s.size == 1);
}

static void test_different_k_and_mode_are_separate_entries() {
    Engine e;
    e.build_from_jsonl(write_corpus("test_cache_c.jsonl", "compiler"));

    e.search("compiler", 5);
    e.search("compiler", 3);                              // different k
    e.search("compiler", 5, search::QueryMode::Or);       // different mode

    const auto s = e.cache_stats();
    ASSERT(s.misses == 3);
    ASSERT(s.hits == 0);
    ASSERT(s.size == 3);
}

static void test_empty_results_are_cached() {
    Engine e;
    e.build_from_jsonl(write_corpus("test_cache_d.jsonl", "compiler"));

    ASSERT(e.search("zzzznotaterm", 5).empty());
    ASSERT(e.search("zzzznotaterm", 5).empty());
    ASSERT(e.cache_stats().hits == 1);  // proving "no matches" is worth caching
}

// The correctness trap: a stale cache fails silently.
static void test_rebuild_invalidates_the_cache() {
    Engine e;
    e.build_from_jsonl(write_corpus("test_cache_e.jsonl", "compiler"));
    ASSERT(!e.search("compiler", 5).empty());

    // Rebuild over a corpus where that word does not occur.
    e.build_from_jsonl(write_corpus("test_cache_f.jsonl", "database"));

    ASSERT(e.search("compiler", 5).empty());     // not the old answer
    ASSERT(!e.search("database", 5).empty());
    ASSERT(e.cache_stats().size <= 2);
}

static void test_load_invalidates_the_cache() {
    const std::string dir = "test_cache_index";
    fs::remove_all(dir);

    Engine saver;
    saver.build_from_jsonl(write_corpus("test_cache_g.jsonl", "database"));
    ASSERT(saver.save(dir));

    Engine e;
    e.build_from_jsonl(write_corpus("test_cache_h.jsonl", "compiler"));
    ASSERT(!e.search("compiler", 5).empty());  // cached against the built index

    ASSERT(e.load(dir));                       // now holding a different corpus
    ASSERT(e.search("compiler", 5).empty());
    ASSERT(!e.search("database", 5).empty());
}

static void test_failed_load_keeps_the_cache_usable() {
    Engine e;
    e.build_from_jsonl(write_corpus("test_cache_i.jsonl", "compiler"));
    const auto before = e.search("compiler", 5);
    ASSERT(!before.empty());

    ASSERT(!e.load("test_cache_no_such_dir"));

    // The index survived the failed load, so the cache must still be correct
    // for it rather than having been cleared or, worse, half-cleared.
    const auto after = e.search("compiler", 5);
    ASSERT(after.size() == before.size());
    ASSERT(after[0].doc_id == before[0].doc_id);
}

static void test_engine_cache_can_be_disabled() {
    Engine e;
    e.set_cache_capacity(0);
    e.build_from_jsonl(write_corpus("test_cache_j.jsonl", "compiler"));

    ASSERT(!e.search("compiler", 5).empty());
    ASSERT(!e.search("compiler", 5).empty());

    const auto s = e.cache_stats();
    ASSERT(s.hits == 0);  // every query recomputed
    ASSERT(s.misses == 2);
    ASSERT(s.size == 0);
}

static void test_concurrent_searches_agree() {
    Engine e;
    e.build_from_jsonl(write_corpus("test_cache_k.jsonl", "compiler"));

    const auto expected = e.search("compiler", 5);
    ASSERT(!expected.empty());

    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&e, &expected, &mismatches]() {
            for (int i = 0; i < 200; ++i) {
                const auto got = e.search("compiler", 5);
                if (got.size() != expected.size()) { ++mismatches; continue; }
                for (std::size_t j = 0; j < got.size(); ++j) {
                    if (got[j].doc_id != expected[j].doc_id
                        || got[j].snippet != expected[j].snippet) {
                        ++mismatches;
                    }
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    ASSERT(mismatches.load() == 0);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "\n=== Query Cache Unit Tests ===\n";

    run_test("key_ignores_query_wording", test_key_ignores_query_wording);
    run_test("key_separates_what_changes_the_result", test_key_separates_what_changes_the_result);
    run_test("key_cannot_be_forged_by_term_content", test_key_cannot_be_forged_by_term_content);

    run_test("miss_then_hit", test_miss_then_hit);
    run_test("hit_rate", test_hit_rate);
    run_test("eviction_is_bounded_by_capacity", test_eviction_is_bounded_by_capacity);
    run_test("eviction_follows_recency_not_insertion", test_eviction_follows_recency_not_insertion);
    run_test("put_replaces_without_growing", test_put_replaces_without_growing);
    run_test("clear_empties_but_keeps_counters", test_clear_empties_but_keeps_counters);
    run_test("capacity_zero_disables_caching", test_capacity_zero_disables_caching);
    run_test("shrinking_capacity_evicts_immediately", test_shrinking_capacity_evicts_immediately);

    run_test("concurrent_access_is_consistent", test_concurrent_access_is_consistent);

    run_test("repeated_query_hits_the_cache", test_repeated_query_hits_the_cache);
    run_test("equivalent_queries_share_an_entry", test_equivalent_queries_share_an_entry);
    run_test("different_k_and_mode_are_separate_entries", test_different_k_and_mode_are_separate_entries);
    run_test("empty_results_are_cached", test_empty_results_are_cached);
    run_test("rebuild_invalidates_the_cache", test_rebuild_invalidates_the_cache);
    run_test("load_invalidates_the_cache", test_load_invalidates_the_cache);
    run_test("failed_load_keeps_the_cache_usable", test_failed_load_keeps_the_cache_usable);
    run_test("engine_cache_can_be_disabled", test_engine_cache_can_be_disabled);
    run_test("concurrent_searches_agree", test_concurrent_searches_agree);

    std::cout << "\n" << g_tests_passed << "/" << g_tests_run << " tests passed.\n\n";
    return (g_tests_passed == g_tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
