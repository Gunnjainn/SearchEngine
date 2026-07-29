// ---------------------------------------------------------------------------
// Unit tests for the Engine class.
// ---------------------------------------------------------------------------

#include "engine.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>

static int g_tests_run    = 0;
static int g_tests_passed = 0;

#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            throw std::runtime_error( \
                std::string("Assertion failed: ") + #cond + \
                " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
        } \
    } while (0)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            throw std::runtime_error( \
                std::string("Assertion failed: ") + #a + " != " + #b + \
                " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
        } \
    } while (0)

void run_test(const std::string& name, void (*test_func)(Engine&), Engine& engine) {
    ++g_tests_run;
    std::cout << "  TEST " << name << " ... ";
    try {
        test_func(engine);
        ++g_tests_passed;
        std::cout << "PASS\n";
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_search_returns_results(Engine& engine) {
    auto results = engine.search("fox", 10);
    ASSERT(results.size() > 0);
}

void test_search_results_sorted_desc(Engine& engine) {
    auto results = engine.search("fox", 10);
    for (size_t i = 1; i < results.size(); ++i) {
        ASSERT(results[i - 1].score >= results[i].score);
    }
}

void test_search_respects_k_limit(Engine& engine) {
    auto results = engine.search("fox", 1);
    ASSERT_EQ(results.size(), static_cast<size_t>(1));
}

void test_result_has_valid_fields(Engine& engine) {
    auto results = engine.search("fox", 10);
    for (const auto& r : results) {
        ASSERT(r.doc_id > 0);
        ASSERT(r.score > 0.0);
        ASSERT(!r.snippet.empty());
    }
}

void test_bm25_ranking(Engine& engine) {
    // Both doc 1 and 2 have 'fox', but doc 1 is shorter and has more 'fox' relative to length
    auto results = engine.search("fox", 10);
    ASSERT(results.size() == 2);
    ASSERT_EQ(results[0].doc_id, 1);
}

// The index build and the query path must run text through the SAME
// tokenizer (search::tokenize), so an inflected query matches an inflected
// document. Doc 4 contains "Running the compilers"; all of these stem to the
// terms actually stored in the index.
void test_shared_tokenizer_matches_inflections(Engine& engine) {
    for (const char* query : {"compile", "compiled", "COMPILERS", "compiling"}) {
        auto results = engine.search(query, 10);
        ASSERT(results.size() == 1);
        ASSERT_EQ(results[0].doc_id, 4);
    }

    auto runs = engine.search("run", 10);
    ASSERT(runs.size() == 1);
    ASSERT_EQ(runs[0].doc_id, 4);

    // "foxes" stems to "fox", so it finds the same docs the bare term does.
    ASSERT_EQ(engine.search("foxes", 10).size(), engine.search("fox", 10).size());
}

// A query made entirely of stopwords has no terms left to score.
void test_stopword_only_query_returns_nothing(Engine& engine) {
    ASSERT(engine.search("the and of", 10).empty());
    ASSERT(engine.search("", 10).empty());
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "\n=== Engine Unit Tests ===\n";
    
    // Create a dummy JSONL file
    std::string temp_jsonl = "test_corpus.jsonl";
    std::ofstream out(temp_jsonl);
    out << "{\"doc_id\": 1, \"title\": \"Fox story\", \"url\": \"x.com\", \"text\": \"The quick brown fox jumps over the lazy dog.\"}\n";
    out << "{\"doc_id\": 2, \"title\": \"Animals\", \"url\": \"y.com\", \"text\": \"A fox is an animal. Dogs are also animals.\"}\n";
    out << "{\"doc_id\": 3, \"title\": \"No match\", \"url\": \"z.com\", \"text\": \"This has nothing to do with the query.\"}\n";
    out << "{\"doc_id\": 4, \"title\": \"Compiler notes\", \"url\": \"w.com\", \"text\": \"Running the compilers requires patience.\"}\n";
    out.close();
    
    Engine engine;
    engine.build_from_jsonl(temp_jsonl);

    run_test("search_returns_results", test_search_returns_results, engine);
    run_test("search_results_sorted_desc", test_search_results_sorted_desc, engine);
    run_test("search_respects_k_limit", test_search_respects_k_limit, engine);
    run_test("result_has_valid_fields", test_result_has_valid_fields, engine);
    run_test("bm25_ranking", test_bm25_ranking, engine);
    run_test("shared_tokenizer_matches_inflections", test_shared_tokenizer_matches_inflections, engine);
    run_test("stopword_only_query_returns_nothing", test_stopword_only_query_returns_nothing, engine);

    std::cout << "\n" << g_tests_passed << "/" << g_tests_run << " tests passed.\n\n";
    return (g_tests_passed == g_tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
