// ---------------------------------------------------------------------------
// Unit tests for query parsing, candidate generation and focused snippets.
//
// The headline test is search_returns_contract2_results — the acceptance
// criterion: real BM25 scores and readable snippets out of Engine::search.
// ---------------------------------------------------------------------------

#include "doc_store.h"
#include "engine.h"
#include "query.h"
#include "tokenizer.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
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

// ── Fixture ──────────────────────────────────────────────────────────────────
//
// doc 1: compiler + database      doc 2: compiler only
// doc 3: database only            doc 4: neither
//
// so "compiler database" under AND matches only doc 1, and under OR matches
// docs 1, 2 and 3.
static std::string write_corpus() {
    const std::string path = "test_query_corpus.jsonl";
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) throw std::runtime_error("cannot write " + path);
    out << R"({"doc_id": 1, "title": "Both", "url": "u1", "text": "The compiler writes to a database every night."})" << "\n";
    out << R"({"doc_id": 2, "title": "Compiler only", "url": "u2", "text": "A compiler compiles source code into machine code."})" << "\n";
    out << R"({"doc_id": 3, "title": "Database only", "url": "u3", "text": "The database stores rows on disk."})" << "\n";
    out << R"({"doc_id": 4, "title": "Neither", "url": "u4", "text": "Kittens sleep in the afternoon sunshine."})" << "\n";
    out.close();
    return path;
}

static const Engine& fixture() {
    static Engine engine;
    static bool built = false;
    if (!built) {
        engine.build_from_jsonl(write_corpus());
        built = true;
    }
    return engine;
}

static std::vector<int> ids(const std::vector<Result>& results) {
    std::vector<int> out;
    for (const Result& r : results) out.push_back(r.doc_id);
    return out;
}

// ---------------------------------------------------------------------------
// Parsing — the query goes through the same tokenizer as indexing
// ---------------------------------------------------------------------------

static void test_parse_uses_the_shared_tokenizer() {
    const search::Query q = search::parse_query("Running the Compilers!");
    ASSERT(q.terms == (std::vector<std::string>{"run", "compil"}));
    ASSERT(q.terms == search::tokenize("Running the Compilers!"));
    ASSERT(q.mode == search::QueryMode::And);  // AND is the default
}

static void test_parse_drops_stopwords_and_punctuation() {
    ASSERT(search::parse_query("the and of").terms.empty());
    ASSERT(search::parse_query("!!! ???").terms.empty());
    ASSERT(search::parse_query("").terms.empty());
}

static void test_parse_keeps_repeated_terms() {
    // BM25 treats a repeated query term as extra weight, so parsing must not
    // silently deduplicate.
    const search::Query q = search::parse_query("compiler compiler");
    ASSERT(q.terms.size() == 2);
    ASSERT(q.terms[0] == q.terms[1]);
}

// ---------------------------------------------------------------------------
// Candidate generation
// ---------------------------------------------------------------------------

static void test_and_requires_every_term() {
    const search::Query q = search::parse_query("compiler database", search::QueryMode::And);
    ASSERT(search::candidate_docs(q, fixture()) == (std::vector<int>{1}));
}

static void test_or_takes_the_union() {
    const search::Query q = search::parse_query("compiler database", search::QueryMode::Or);
    ASSERT(search::candidate_docs(q, fixture()) == (std::vector<int>{1, 2, 3}));
}

static void test_single_term_modes_agree() {
    const auto a = search::candidate_docs(
        search::parse_query("compiler", search::QueryMode::And), fixture());
    const auto o = search::candidate_docs(
        search::parse_query("compiler", search::QueryMode::Or), fixture());
    ASSERT(a == (std::vector<int>{1, 2}));
    ASSERT(a == o);
}

static void test_and_with_one_absent_term_is_empty() {
    // "compiler" exists, "zzzznotaterm" does not, so nothing can contain both.
    const search::Query q = search::parse_query("compiler zzzznotaterm");
    ASSERT(search::candidate_docs(q, fixture()).empty());
}

static void test_or_ignores_absent_terms() {
    const search::Query q = search::parse_query("compiler zzzznotaterm", search::QueryMode::Or);
    ASSERT(search::candidate_docs(q, fixture()) == (std::vector<int>{1, 2}));
}

static void test_candidates_ascend_and_are_unique() {
    // A repeated term must not produce a document twice.
    const search::Query q = search::parse_query("compiler compiler", search::QueryMode::Or);
    const std::vector<int> got = search::candidate_docs(q, fixture());
    ASSERT(got == (std::vector<int>{1, 2}));
    for (std::size_t i = 1; i < got.size(); ++i) ASSERT(got[i - 1] < got[i]);
}

static void test_empty_query_has_no_candidates() {
    ASSERT(search::candidate_docs(search::parse_query("the and of"), fixture()).empty());
    ASSERT(search::candidate_docs(search::parse_query(""), fixture()).empty());
}

// ---------------------------------------------------------------------------
// search() end to end
// ---------------------------------------------------------------------------

// The acceptance criterion.
static void test_search_returns_contract2_results() {
    const auto results = fixture().search("compiler", 10);
    ASSERT(results.size() == 2);

    for (const Result& r : results) {
        ASSERT(r.doc_id > 0);
        ASSERT(r.score > 0.0);          // a real BM25 score, not a placeholder
        ASSERT(!r.snippet.empty());     // a readable snippet
    }
    // Sorted by score DESC.
    ASSERT(results[0].score >= results[1].score);
}

static void test_search_defaults_to_and() {
    // Only doc 1 has both terms, so AND must not return docs 2 and 3.
    ASSERT(ids(fixture().search("compiler database", 10)) == (std::vector<int>{1}));
}

static void test_search_or_mode_widens_the_result() {
    const auto results = fixture().search("compiler database", 10, search::QueryMode::Or);
    ASSERT(results.size() == 3);
    // Doc 1 has both terms, so it must outrank the single-term matches.
    ASSERT(results[0].doc_id == 1);
}

static void test_search_multi_term_ranks_by_bm25() {
    const auto results = fixture().search("compiler database", 10, search::QueryMode::Or);
    for (std::size_t i = 1; i < results.size(); ++i) {
        ASSERT(results[i - 1].score >= results[i].score);
    }
    ASSERT(results.front().score > results.back().score);
}

static void test_search_respects_k() {
    ASSERT(fixture().search("compiler database", 1, search::QueryMode::Or).size() == 1);
    ASSERT(fixture().search("compiler database", 2, search::QueryMode::Or).size() == 2);
}

static void test_search_empty_and_stopword_queries() {
    ASSERT(fixture().search("", 10).empty());
    ASSERT(fixture().search("the and of", 10).empty());
    ASSERT(fixture().search("zzzznotaterm", 10).empty());
}

static void test_search_survives_a_round_trip_to_disk() {
    const std::string dir = "test_query_index";
    fs::remove_all(dir);

    Engine built;
    built.build_from_jsonl(write_corpus());
    ASSERT(built.save(dir));

    Engine loaded;
    ASSERT(loaded.load(dir));

    const auto a = built.search("compiler database", 10, search::QueryMode::Or);
    const auto b = loaded.search("compiler database", 10, search::QueryMode::Or);
    ASSERT(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        ASSERT(a[i].doc_id == b[i].doc_id);
        ASSERT(a[i].score == b[i].score);
        ASSERT(a[i].snippet == b[i].snippet);
    }
}

// ---------------------------------------------------------------------------
// Focused snippets
// ---------------------------------------------------------------------------

static void test_snippet_shows_the_match_not_the_head() {
    // The query term sits far past the 150-byte mark, so a head snippet would
    // miss it entirely.
    const std::string text =
        std::string(400, 'a') + " the quick brown compiler jumps over " + std::string(400, 'b');

    const std::string snippet = search::make_focused_snippet(text, {"compil"}, 150);
    ASSERT(snippet.find("compiler") != std::string::npos);
    ASSERT(snippet.rfind("...", 0) == 0);  // truncated on the left
    ASSERT(snippet.size() <= 150 + 6);     // content plus both ellipses
}

static void test_snippet_prefers_the_window_with_most_distinct_terms() {
    // Two candidate windows: one with a lone "compiler", one where "compiler"
    // and "database" sit together. The denser window should win.
    const std::string text =
        "compiler " + std::string(300, 'x') + " compiler database " + std::string(300, 'y');

    const std::string snippet = search::make_focused_snippet(text, {"compil", "databas"}, 150);
    ASSERT(snippet.find("compiler database") != std::string::npos);
}

static void test_snippet_falls_back_when_no_term_matches() {
    const std::string text(400, 'a');
    const std::string snippet = search::make_focused_snippet(text, {"compil"}, 150);
    ASSERT(snippet == search::make_snippet(text, 150));
}

static void test_short_document_is_returned_whole() {
    const std::string text = "A compiler compiles code.";
    ASSERT(search::make_focused_snippet(text, {"compil"}, 150) == text);
    // No ellipsis when nothing was cut.
    ASSERT(search::make_focused_snippet(text, {"compil"}, 150).find("...") == std::string::npos);
}

static void test_snippet_never_splits_a_utf8_character() {
    // Accented text either side of the match; every cut must land on a
    // character boundary or nlohmann's dump() would throw on the response.
    const std::string pad = "caf\xC3\xA9 na\xC3\xAFve fa\xC3\xA7" "ade ";
    std::string text;
    for (int i = 0; i < 40; ++i) text += pad;
    text += "compiler ";
    for (int i = 0; i < 40; ++i) text += pad;

    for (std::size_t width = 40; width <= 200; width += 7) {
        const std::string snippet = search::make_focused_snippet(text, {"compil"}, width);
        // No trailing continuation byte, and no byte that starts a sequence
        // whose continuation bytes were cut off.
        for (std::size_t i = 0; i < snippet.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(snippet[i]);
            if ((c & 0x80) == 0) continue;                  // ASCII
            if ((c & 0xC0) == 0x80) continue;               // continuation
            const std::size_t need = ((c & 0xE0) == 0xC0) ? 1 : ((c & 0xF0) == 0xE0) ? 2 : 3;
            ASSERT(i + need < snippet.size());
            for (std::size_t n = 1; n <= need; ++n) {
                ASSERT((static_cast<unsigned char>(snippet[i + n]) & 0xC0) == 0x80);
            }
            i += need;
        }
    }
}

static void test_snippet_handles_a_token_longer_than_the_window() {
    // One unbroken 5000-character token: there is no space to snap to, so the
    // plain byte cut has to stand rather than collapsing the snippet.
    const std::string text = "compiler" + std::string(5000, 'z');
    const std::string snippet = search::make_focused_snippet(text, {"compil"}, 150);
    ASSERT(snippet.size() >= 150);
    ASSERT(snippet.size() <= 156);
}

static void test_search_snippet_contains_the_query_term() {
    const std::string json = "test_query_longdoc.jsonl";
    {
        std::ofstream out(json, std::ios::binary);
        out << R"({"doc_id": 1, "title": "Long", "url": "u", "text": ")"
            << std::string(500, 'a') << " the compiler appears late in this document "
            << std::string(500, 'b') << R"("})" << "\n";
    }

    Engine e;
    e.build_from_jsonl(json);
    const auto results = e.search("compiler", 10);
    ASSERT(results.size() == 1);
    ASSERT(results[0].snippet.find("compiler") != std::string::npos);
}

// ---------------------------------------------------------------------------
// tokenize_spans agrees with tokenize
// ---------------------------------------------------------------------------

static void test_spans_agree_with_tokenize() {
    const std::vector<std::string> inputs = {
        "Running the Compilers!",
        "Caf\xC3\xA9 NA\xC3\x8FVE Z\xC3\xBCrich",
        "snake_case kebab-case don't",
        "BM25 scores 42 docs",
        "",
        "   ",
        "the and of a an",
        "cafe\xCC\x81 combining marks",
    };

    for (const std::string& in : inputs) {
        const auto spans = search::tokenize_spans(in);
        std::vector<std::string> terms;
        for (const auto& s : spans) terms.push_back(s.term);
        if (terms != search::tokenize(in)) {
            throw std::runtime_error("tokenize_spans disagrees with tokenize for \"" + in + "\"");
        }
    }
}

static void test_spans_point_at_the_source_text() {
    const std::string text = "The quick brown compiler jumps";
    const auto spans = search::tokenize_spans(text);
    ASSERT(!spans.empty());

    for (const auto& s : spans) {
        ASSERT(s.begin < s.end);
        ASSERT(s.end <= text.size());
    }

    // The span for "compil" must cover the word "compiler" in the source.
    bool found = false;
    for (const auto& s : spans) {
        if (s.term == "compil") {
            ASSERT(text.substr(s.begin, s.end - s.begin) == "compiler");
            found = true;
        }
    }
    ASSERT(found);
}

static void test_spans_survive_diacritic_folding() {
    // "Café" folds to "cafe" — four characters, five bytes — so the span must
    // describe the source range, not the folded length.
    const std::string text = "x Caf\xC3\xA9 y";
    const auto spans = search::tokenize_spans(text);
    bool found = false;
    for (const auto& s : spans) {
        if (s.term == "cafe") {
            ASSERT(text.substr(s.begin, s.end - s.begin) == "Caf\xC3\xA9");
            found = true;
        }
    }
    ASSERT(found);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "\n=== Query Unit Tests ===\n";

    run_test("parse_uses_the_shared_tokenizer", test_parse_uses_the_shared_tokenizer);
    run_test("parse_drops_stopwords_and_punctuation", test_parse_drops_stopwords_and_punctuation);
    run_test("parse_keeps_repeated_terms", test_parse_keeps_repeated_terms);

    run_test("and_requires_every_term", test_and_requires_every_term);
    run_test("or_takes_the_union", test_or_takes_the_union);
    run_test("single_term_modes_agree", test_single_term_modes_agree);
    run_test("and_with_one_absent_term_is_empty", test_and_with_one_absent_term_is_empty);
    run_test("or_ignores_absent_terms", test_or_ignores_absent_terms);
    run_test("candidates_ascend_and_are_unique", test_candidates_ascend_and_are_unique);
    run_test("empty_query_has_no_candidates", test_empty_query_has_no_candidates);

    run_test("search_returns_contract2_results", test_search_returns_contract2_results);
    run_test("search_defaults_to_and", test_search_defaults_to_and);
    run_test("search_or_mode_widens_the_result", test_search_or_mode_widens_the_result);
    run_test("search_multi_term_ranks_by_bm25", test_search_multi_term_ranks_by_bm25);
    run_test("search_respects_k", test_search_respects_k);
    run_test("search_empty_and_stopword_queries", test_search_empty_and_stopword_queries);
    run_test("search_survives_a_round_trip_to_disk", test_search_survives_a_round_trip_to_disk);

    run_test("snippet_shows_the_match_not_the_head", test_snippet_shows_the_match_not_the_head);
    run_test("snippet_prefers_the_window_with_most_distinct_terms", test_snippet_prefers_the_window_with_most_distinct_terms);
    run_test("snippet_falls_back_when_no_term_matches", test_snippet_falls_back_when_no_term_matches);
    run_test("short_document_is_returned_whole", test_short_document_is_returned_whole);
    run_test("snippet_never_splits_a_utf8_character", test_snippet_never_splits_a_utf8_character);
    run_test("snippet_handles_a_token_longer_than_the_window", test_snippet_handles_a_token_longer_than_the_window);
    run_test("search_snippet_contains_the_query_term", test_search_snippet_contains_the_query_term);

    run_test("spans_agree_with_tokenize", test_spans_agree_with_tokenize);
    run_test("spans_point_at_the_source_text", test_spans_point_at_the_source_text);
    run_test("spans_survive_diacritic_folding", test_spans_survive_diacritic_folding);

    std::cout << "\n" << g_tests_passed << "/" << g_tests_run << " tests passed.\n\n";
    return (g_tests_passed == g_tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
