// ---------------------------------------------------------------------------
// Unit tests for the BM25 scorer (bm25.h / bm25.cpp).
//
// All tests use a hand-built in-memory postings stub (StubSource) so that
// BM25 can be verified without touching the real Engine or a JSONL file.
//
// Fixture (same 3 docs as test_index.cpp):
//   Doc 10: length=6  terms: compil(4), code(1), tool(1)
//   Doc 20: length=5  terms: databas(2), index(1), make(1), fast(1)
//   Doc 30: length=6  terms: compil(2), note(1), run(1), requir(1), patienc(1)
//
// N = 3, total_tokens = 17, avgdl = 17/3.
// k1 = 1.2, b = 0.75  (BM25Params defaults).
//
// IDF formula:   idf = ln(1 + (N - df + 0.5) / (df + 0.5))
// TF saturation: idf * (tf * (k1+1)) / (tf + k1*(1 - b + b*dl/avgdl))
//
// Hand calculations below are exact rational arithmetic carried to double.
// ---------------------------------------------------------------------------

#include "bm25.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

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

// Floating point comparison within tolerance.
static void assert_near(double actual, double expected, double tol,
                        const std::string& label) {
    if (std::fabs(actual - expected) > tol) {
        std::ostringstream os;
        os.precision(15);
        os << label << ": expected " << expected << ", got " << actual
           << " (diff " << std::fabs(actual - expected) << ", tol " << tol << ")";
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
// StubSource — hand-built in-memory postings for the 3-doc fixture.
// ---------------------------------------------------------------------------

class StubSource : public PostingsSource {
public:
    StubSource() {
        // Doc 10: compil(4), code(1), tool(1)   length=6
        // Doc 20: databas(2), index(1), make(1), fast(1)  length=5
        // Doc 30: compil(2), note(1), run(1), requir(1), patienc(1)  length=6
        index_["compil"]  = {{10, 4}, {30, 2}};  // ascending by doc_id
        index_["code"]    = {{10, 1}};
        index_["tool"]    = {{10, 1}};
        index_["databas"] = {{20, 2}};
        index_["index"]   = {{20, 1}};
        index_["make"]    = {{20, 1}};
        index_["fast"]    = {{20, 1}};
        index_["note"]    = {{30, 1}};
        index_["run"]     = {{30, 1}};
        index_["requir"]  = {{30, 1}};
        index_["patienc"] = {{30, 1}};

        lengths_[10] = 6;
        lengths_[20] = 5;
        lengths_[30] = 6;
    }

    const std::vector<Posting>& postings(const std::string& term) const override {
        auto it = index_.find(term);
        return (it == index_.end()) ? empty_ : it->second;
    }

    int doc_length(int doc_id) const override {
        auto it = lengths_.find(doc_id);
        return (it == lengths_.end()) ? 0 : it->second;
    }

    int num_docs() const override { return 3; }

    double avg_doc_length() const override {
        return 17.0 / 3.0;
    }

private:
    std::unordered_map<std::string, std::vector<Posting>> index_;
    std::unordered_map<int, int> lengths_;
    std::vector<Posting> empty_;
};

// ---------------------------------------------------------------------------
// Hand-computed BM25 values
//
// Constants:
//   k1    = 1.2
//   b     = 0.75
//   N     = 3
//   avgdl = 17/3
//
// Helper notation:
//   norm(dl) = k1 * (1 - b + b * dl/avgdl) = 1.2 * (0.25 + 0.75 * 3*dl/17)
//
//   For dl=6: norm = 1.2 * (0.25 + 0.75*18/17) = 1.2 * (0.25 + 13.5/17)
//           = 1.2 * (4.25/17 + 13.5/17) = 1.2 * 17.75/17 = 21.3/17
//
//   For dl=5: norm = 1.2 * (0.25 + 0.75*15/17) = 1.2 * (4.25/17 + 11.25/17)
//           = 1.2 * 15.5/17 = 18.6/17
// ---------------------------------------------------------------------------

static const double NORM_6  = 21.3 / 17.0;   // ≈ 1.252941176
static const double NORM_5  = 18.6 / 17.0;   // ≈ 1.094117647

// idf("compil"): df=2  -> ln(1 + (3-2+0.5)/(2+0.5)) = ln(1 + 1.5/2.5) = ln(1.6)
static const double IDF_COMPIL = std::log(1.6);

// idf("databas"): df=1  -> ln(1 + (3-1+0.5)/(1+0.5)) = ln(1 + 2.5/1.5) = ln(8/3)
static const double IDF_DATABAS = std::log(8.0 / 3.0);

// idf for any term with df=1: same as IDF_DATABAS
static const double IDF_DF1 = IDF_DATABAS;

// BM25 contribution for a single term:
//   idf * tf * (k1+1) / (tf + norm)
//   = idf * tf * 2.2 / (tf + norm)

static double bm25_term(double idf, double tf, double norm) {
    return idf * (tf * 2.2) / (tf + norm);
}

// Pre-computed expected scores for single-term queries.
static const double SCORE_COMPIL_DOC10 = bm25_term(IDF_COMPIL, 4.0, NORM_6);
static const double SCORE_COMPIL_DOC30 = bm25_term(IDF_COMPIL, 2.0, NORM_6);
static const double SCORE_DATABAS_DOC20 = bm25_term(IDF_DATABAS, 2.0, NORM_5);
static const double SCORE_CODE_DOC10 = bm25_term(IDF_DF1, 1.0, NORM_6);

static const double TOL = 1e-12;

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_single_term_compil() {
    StubSource src;
    auto scores = score_docs({"compil"}, src);

    ASSERT(scores.size() == 2);
    assert_near(scores[10], SCORE_COMPIL_DOC10, TOL, "compil doc 10");
    assert_near(scores[30], SCORE_COMPIL_DOC30, TOL, "compil doc 30");
}

static void test_single_term_databas() {
    StubSource src;
    auto scores = score_docs({"databas"}, src);

    ASSERT(scores.size() == 1);
    assert_near(scores[20], SCORE_DATABAS_DOC20, TOL, "databas doc 20");
}

static void test_single_term_code() {
    StubSource src;
    auto scores = score_docs({"code"}, src);

    ASSERT(scores.size() == 1);
    assert_near(scores[10], SCORE_CODE_DOC10, TOL, "code doc 10");
}

static void test_higher_tf_means_higher_score() {
    // "compil" in doc 10 (tf=4) should score higher than doc 30 (tf=2),
    // since both have the same doc length.
    StubSource src;
    auto scores = score_docs({"compil"}, src);
    ASSERT(scores[10] > scores[30]);
}

static void test_rarer_term_has_higher_idf() {
    // "code" (df=1) has a higher IDF than "compil" (df=2). Given tf=1 and
    // the same doc length (6), "code" should score higher than "compil"
    // would with tf=1 in the same document.
    //
    // We compare BM25 for tf=1, dl=6 across the two IDFs.
    double score_rare   = bm25_term(IDF_DF1,     1.0, NORM_6);
    double score_common = bm25_term(IDF_COMPIL,  1.0, NORM_6);
    ASSERT(score_rare > score_common);
}

static void test_multi_term_query_adds_scores() {
    // query {"compil", "code"}: doc 10 gets both contributions,
    // doc 30 gets only "compil".
    StubSource src;
    auto scores = score_docs({"compil", "code"}, src);

    ASSERT(scores.size() == 2);  // docs 10 and 30
    assert_near(scores[10], SCORE_COMPIL_DOC10 + SCORE_CODE_DOC10, TOL,
                "compil+code doc 10");
    assert_near(scores[30], SCORE_COMPIL_DOC30, TOL,
                "compil+code doc 30 (no code)");
}

static void test_unknown_term_contributes_nothing() {
    StubSource src;
    auto scores = score_docs({"zzzznotaterm"}, src);
    ASSERT(scores.empty());
}

static void test_empty_query_returns_empty() {
    StubSource src;
    auto scores = score_docs({}, src);
    ASSERT(scores.empty());
}

static void test_duplicate_query_term_double_counts() {
    // If the same term appears twice in the query, its BM25 contribution
    // is added twice. This matches standard BM25 behaviour.
    StubSource src;
    auto scores_single = score_docs({"compil"}, src);
    auto scores_double = score_docs({"compil", "compil"}, src);

    assert_near(scores_double[10], 2.0 * scores_single[10], TOL,
                "double compil doc 10");
    assert_near(scores_double[30], 2.0 * scores_single[30], TOL,
                "double compil doc 30");
}

static void test_custom_params() {
    StubSource src;
    BM25Params params{1.5, 0.5};  // non-default k1 and b

    auto scores_default = score_docs({"compil"}, src);
    auto scores_custom  = score_docs({"compil"}, src, params);

    // With different parameters, scores should differ.
    ASSERT(std::fabs(scores_default[10] - scores_custom[10]) > 1e-6);

    // Hand-compute for the custom params:
    // norm_custom(dl=6) = 1.5*(1-0.5+0.5*18/17) = 1.5*(0.5 + 9/17)
    //                   = 1.5*(8.5/17 + 9/17) = 1.5*17.5/17 = 26.25/17
    double norm_custom_6 = 26.25 / 17.0;
    double idf = IDF_COMPIL;
    double expected_10 = idf * (4.0 * 2.5) / (4.0 + norm_custom_6);
    double expected_30 = idf * (2.0 * 2.5) / (2.0 + norm_custom_6);
    assert_near(scores_custom[10], expected_10, TOL, "custom params doc 10");
    assert_near(scores_custom[30], expected_30, TOL, "custom params doc 30");
}

static void test_zero_avgdl_returns_empty() {
    // Edge case: a corpus where every document tokenized to nothing.
    class EmptyCorpus : public PostingsSource {
    public:
        const std::vector<Posting>& postings(const std::string&) const override {
            return empty_;
        }
        int doc_length(int) const override { return 0; }
        int num_docs() const override { return 2; }
        double avg_doc_length() const override { return 0.0; }
    private:
        std::vector<Posting> empty_;
    };

    EmptyCorpus src;
    auto scores = score_docs({"compil"}, src);
    ASSERT(scores.empty());
}

static void test_no_docs_returns_empty() {
    class NoDocs : public PostingsSource {
    public:
        const std::vector<Posting>& postings(const std::string&) const override {
            return empty_;
        }
        int doc_length(int) const override { return 0; }
        int num_docs() const override { return 0; }
        double avg_doc_length() const override { return 0.0; }
    private:
        std::vector<Posting> empty_;
    };

    NoDocs src;
    auto scores = score_docs({"compil"}, src);
    ASSERT(scores.empty());
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    std::cout << "\n=== BM25 Unit Tests ===\n";

    run_test("single_term_compil",               test_single_term_compil);
    run_test("single_term_databas",              test_single_term_databas);
    run_test("single_term_code",                 test_single_term_code);
    run_test("higher_tf_means_higher_score",     test_higher_tf_means_higher_score);
    run_test("rarer_term_has_higher_idf",        test_rarer_term_has_higher_idf);
    run_test("multi_term_query_adds_scores",     test_multi_term_query_adds_scores);
    run_test("unknown_term_contributes_nothing", test_unknown_term_contributes_nothing);
    run_test("empty_query_returns_empty",        test_empty_query_returns_empty);
    run_test("duplicate_query_term_double_counts", test_duplicate_query_term_double_counts);
    run_test("custom_params",                    test_custom_params);
    run_test("zero_avgdl_returns_empty",         test_zero_avgdl_returns_empty);
    run_test("no_docs_returns_empty",            test_no_docs_returns_empty);

    std::cout << "\n" << g_tests_passed << "/" << g_tests_run
              << " tests passed.\n\n";
    return (g_tests_passed == g_tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
