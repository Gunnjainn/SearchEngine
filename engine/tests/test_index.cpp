// ---------------------------------------------------------------------------
// Unit tests for the inverted index and the SYNC POINT 2a accessors:
// postings(), doc_length(), num_docs(), avg_doc_length(), doc_text(),
// doc_meta().
//
// The fixture lists documents OUT of doc_id order (30, 10, 20) on purpose, so
// that a postings list which merely preserved insertion order would fail the
// ascending-by-doc_id checks.
//
// Expected token counts and term frequencies are derived by hand from the
// tokenizer's documented behaviour, e.g. for doc 10:
//   "Compilers" + " " + "A compiler compiles code. Compilers are tools."
//     -> fold/split: compilers a compiler compiles code compilers are tools
//     -> stopwords:  (a, are dropped)
//     -> stems:      compil compil compil code compil tool
//   so doc_length(10) == 6 and postings("compil") has {10, 4}.
// ---------------------------------------------------------------------------

#include "engine.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

// ── Fixture ──────────────────────────────────────────────────────────────────

static std::string write_fixture() {
    const std::string path = "test_index_corpus.jsonl";
    std::ofstream out(path);
    if (!out.is_open()) throw std::runtime_error("cannot write " + path);

    out << R"({"doc_id": 30, "title": "Compiler notes", "url": "http://e.test/30", "text": "Running the compilers requires patience."})" << "\n";
    out << R"({"doc_id": 10, "title": "Compilers", "url": "http://e.test/10", "text": "A compiler compiles code. Compilers are tools."})" << "\n";
    out << R"({"doc_id": 20, "title": "Databases", "url": "http://e.test/20", "text": "Indexes make databases fast."})" << "\n";
    out.close();
    return path;
}

static const Engine& fixture() {
    static Engine engine;
    static bool built = false;
    if (!built) {
        engine.build_from_jsonl(write_fixture());
        built = true;
    }
    return engine;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string show(const std::vector<Posting>& postings) {
    std::ostringstream os;
    os << "[";
    for (std::size_t i = 0; i < postings.size(); ++i) {
        if (i) os << ", ";
        os << "{" << postings[i].doc_id << ", " << postings[i].term_freq << "}";
    }
    os << "]";
    return os.str();
}

// Assert the exact postings list for `term`, as {doc_id, term_freq} pairs.
static void expect_postings(const std::string& term,
                            const std::vector<std::pair<int, int>>& expected) {
    const std::vector<Posting>& actual = fixture().postings(term);

    bool same = actual.size() == expected.size();
    for (std::size_t i = 0; same && i < actual.size(); ++i) {
        same = actual[i].doc_id == expected[i].first
            && actual[i].term_freq == expected[i].second;
    }
    if (!same) {
        std::ostringstream os;
        os << "postings(\"" << term << "\") -> " << show(actual) << ", expected [";
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (i) os << ", ";
            os << "{" << expected[i].first << ", " << expected[i].second << "}";
        }
        os << "]";
        throw std::runtime_error(os.str());
    }
}

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
// Acceptance criteria
// ---------------------------------------------------------------------------

static void test_acceptance_postings_compil() {
    // doc 10 has compilers/compiler/compiles/Compilers -> compil x4
    // doc 30 has Compiler/compilers                    -> compil x2
    // Ascending by doc_id even though doc 30 was indexed first.
    expect_postings("compil", {{10, 4}, {30, 2}});
}

static void test_acceptance_collection_stats() {
    const Engine& e = fixture();
    ASSERT(e.num_docs() == 3);
    // Token counts: doc 10 -> 6, doc 20 -> 5, doc 30 -> 6. Total 17.
    const double expected = 17.0 / 3.0;
    ASSERT(std::fabs(e.avg_doc_length() - expected) < 1e-12);
}

// ---------------------------------------------------------------------------
// Term dictionary and postings
// ---------------------------------------------------------------------------

static void test_term_dictionary_size() {
    // doc 30: compil note run requir patienc
    // doc 10: compil code tool
    // doc 20: databas index make fast
    // union  = 11 distinct terms
    ASSERT(fixture().num_terms() == 11);
}

static void test_term_freq_counts_repeats() {
    expect_postings("databas", {{20, 2}});   // title "Databases" + text "databases"
    expect_postings("code", {{10, 1}});
    expect_postings("tool", {{10, 1}});
    expect_postings("patienc", {{30, 1}});
}

static void test_postings_ascend_by_doc_id_for_every_term() {
    const Engine& e = fixture();
    for (const char* term : {"compil", "note", "run", "requir", "patienc",
                             "code", "tool", "databas", "index", "make", "fast"}) {
        const std::vector<Posting>& plist = e.postings(term);
        ASSERT(!plist.empty());
        for (std::size_t i = 1; i < plist.size(); ++i) {
            ASSERT(plist[i - 1].doc_id < plist[i].doc_id);  // sorted AND unique
        }
    }
}

static void test_one_posting_per_document() {
    // "compil" occurs 4 times in doc 10 but must appear as a single posting.
    const std::vector<Posting>& plist = fixture().postings("compil");
    ASSERT(plist.size() == 2);
    for (const Posting& p : plist) {
        ASSERT(p.term_freq > 0);
    }
}

static void test_unknown_term_returns_empty_list() {
    ASSERT(fixture().postings("zzzznotaterm").empty());
    ASSERT(fixture().postings("").empty());
    // Stopwords and unstemmed surface forms are not index terms.
    ASSERT(fixture().postings("the").empty());
    ASSERT(fixture().postings("compilers").empty());
}

// ---------------------------------------------------------------------------
// Per-document statistics
// ---------------------------------------------------------------------------

static void test_doc_length() {
    const Engine& e = fixture();
    ASSERT(e.doc_length(10) == 6);
    ASSERT(e.doc_length(20) == 5);
    ASSERT(e.doc_length(30) == 6);
}

static void test_doc_length_sums_to_total_tokens() {
    const Engine& e = fixture();
    const int total = e.doc_length(10) + e.doc_length(20) + e.doc_length(30);
    ASSERT(std::fabs(e.avg_doc_length() * e.num_docs() - total) < 1e-9);
}

static void test_unknown_doc_id_is_zero_length() {
    ASSERT(fixture().doc_length(999) == 0);
    ASSERT(fixture().doc_length(-1) == 0);
    ASSERT(fixture().doc_length(0) == 0);
}

// ---------------------------------------------------------------------------
// Doc store
// ---------------------------------------------------------------------------

static void test_doc_meta_and_text() {
    const Engine& e = fixture();

    ASSERT(e.doc_meta(10).title == "Compilers");
    ASSERT(e.doc_meta(10).url == "http://e.test/10");
    ASSERT(e.doc_meta(30).title == "Compiler notes");
    ASSERT(e.doc_meta(20).url == "http://e.test/20");

    ASSERT(e.doc_text(30) == "Running the compilers requires patience.");
    ASSERT(e.doc_text(20) == "Indexes make databases fast.");
}

static void test_doc_store_unknown_id_is_empty() {
    const Engine& e = fixture();
    ASSERT(e.doc_text(999).empty());
    ASSERT(e.doc_meta(999).title.empty());
    ASSERT(e.doc_meta(999).url.empty());
}

// ---------------------------------------------------------------------------
// Build behaviour
// ---------------------------------------------------------------------------

static void test_empty_index_reports_zero_stats() {
    Engine e;
    ASSERT(e.num_docs() == 0);
    ASSERT(e.num_terms() == 0);
    ASSERT(e.avg_doc_length() == 0.0);  // not NaN — nothing divides by zero
    ASSERT(e.postings("compil").empty());
    ASSERT(e.search("compil", 10).empty());
}

static void test_duplicate_doc_id_keeps_first() {
    const std::string path = "test_index_dupes.jsonl";
    std::ofstream out(path);
    out << R"({"doc_id": 7, "title": "First", "url": "http://e.test/a", "text": "compiler compiler"})" << "\n";
    out << R"({"doc_id": 7, "title": "Second", "url": "http://e.test/b", "text": "database"})" << "\n";
    out.close();

    Engine e;
    e.build_from_jsonl(path);

    ASSERT(e.num_docs() == 1);
    ASSERT(e.doc_meta(7).title == "First");
    ASSERT(e.postings("databas").empty());

    // The kept document indexes as {first, compil, compil}, so "compil" has a
    // single posting with tf 2 — the skipped line contributed nothing.
    const std::vector<Posting>& plist = e.postings("compil");
    ASSERT(plist.size() == 1);
    ASSERT(plist[0].doc_id == 7);
    ASSERT(plist[0].term_freq == 2);
}

static void test_rebuild_replaces_index() {
    Engine e;
    e.build_from_jsonl(write_fixture());
    const int docs_once  = e.num_docs();
    const std::size_t terms_once = e.num_terms();
    const double avg_once = e.avg_doc_length();

    e.build_from_jsonl(write_fixture());  // same corpus again

    ASSERT(e.num_docs() == docs_once);          // not doubled
    ASSERT(e.num_terms() == terms_once);
    ASSERT(std::fabs(e.avg_doc_length() - avg_once) < 1e-12);
    expect_postings("compil", {{10, 4}, {30, 2}});
}

// ---------------------------------------------------------------------------
// The index is what search reads
// ---------------------------------------------------------------------------

static void test_search_uses_the_postings_lists() {
    const Engine& e = fixture();

    // "compile" stems to "compil", whose postings are docs 10 and 30.
    auto results = e.search("compile", 10);
    ASSERT(results.size() == 2);

    // doc 10 has tf 4 in 6 tokens, doc 30 has tf 2 in 6 tokens -> doc 10 wins.
    ASSERT(results[0].doc_id == 10);
    ASSERT(results[1].doc_id == 30);
    ASSERT(results[0].score > results[1].score);

    // Snippet comes from the doc store, not the index.
    ASSERT(results[1].snippet == e.doc_text(30));
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "\n=== Index Unit Tests ===\n";

    run_test("acceptance_postings_compil", test_acceptance_postings_compil);
    run_test("acceptance_collection_stats", test_acceptance_collection_stats);

    run_test("term_dictionary_size", test_term_dictionary_size);
    run_test("term_freq_counts_repeats", test_term_freq_counts_repeats);
    run_test("postings_ascend_by_doc_id_for_every_term", test_postings_ascend_by_doc_id_for_every_term);
    run_test("one_posting_per_document", test_one_posting_per_document);
    run_test("unknown_term_returns_empty_list", test_unknown_term_returns_empty_list);

    run_test("doc_length", test_doc_length);
    run_test("doc_length_sums_to_total_tokens", test_doc_length_sums_to_total_tokens);
    run_test("unknown_doc_id_is_zero_length", test_unknown_doc_id_is_zero_length);

    run_test("doc_meta_and_text", test_doc_meta_and_text);
    run_test("doc_store_unknown_id_is_empty", test_doc_store_unknown_id_is_empty);

    run_test("empty_index_reports_zero_stats", test_empty_index_reports_zero_stats);
    run_test("duplicate_doc_id_keeps_first", test_duplicate_doc_id_keeps_first);
    run_test("rebuild_replaces_index", test_rebuild_replaces_index);

    run_test("search_uses_the_postings_lists", test_search_uses_the_postings_lists);

    std::cout << "\n" << g_tests_passed << "/" << g_tests_run << " tests passed.\n\n";
    return (g_tests_passed == g_tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
