// ---------------------------------------------------------------------------
// Unit tests for the shared text pipeline (search::tokenize) and the Porter
// stemmer underneath it.
//
// Known input -> known token output. Expected stems were taken from Porter's
// published algorithm and reference output, not from this implementation.
// ---------------------------------------------------------------------------

#include "tokenizer.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "porter.h"

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

static std::string show(const std::vector<std::string>& v) {
    std::ostringstream os;
    os << "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) os << ", ";
        os << '"' << v[i] << '"';
    }
    os << "]";
    return os.str();
}

static void expect_tokens(const std::string& input,
                          const std::vector<std::string>& expected) {
    const auto actual = search::tokenize(input);
    if (actual != expected) {
        throw std::runtime_error("tokenize(\"" + input + "\") -> " +
                                 show(actual) + ", expected " + show(expected));
    }
}

static void expect_stem(const std::string& word, const std::string& expected) {
    const std::string actual = search::porter_stem(word);
    if (actual != expected) {
        throw std::runtime_error("porter_stem(\"" + word + "\") -> \"" +
                                 actual + "\", expected \"" + expected + "\"");
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
// Acceptance criterion
// ---------------------------------------------------------------------------

static void test_acceptance_running_the_compilers() {
    // "the" is a stopword; "Running" -> "run"; "Compilers" -> "compil".
    expect_tokens("Running the Compilers!", {"run", "compil"});
}

// ---------------------------------------------------------------------------
// Stage 1+2: folding and splitting
// ---------------------------------------------------------------------------

static void test_lowercases_ascii() {
    ASSERT(search::fold_and_split("HeLLo WORLD") ==
           (std::vector<std::string>{"hello", "world"}));
}

static void test_splits_on_non_alphanumeric() {
    ASSERT(search::fold_and_split("foo,bar;baz\tqux\nquux") ==
           (std::vector<std::string>{"foo", "bar", "baz", "qux", "quux"}));
    // Underscores, hyphens and apostrophes are separators too.
    ASSERT(search::fold_and_split("snake_case kebab-case don't") ==
           (std::vector<std::string>{"snake", "case", "kebab", "case", "don", "t"}));
}

static void test_keeps_digits_and_alphanumeric_runs() {
    ASSERT(search::fold_and_split("BM25 scores 42 docs") ==
           (std::vector<std::string>{"bm25", "scores", "42", "docs"}));
}

static void test_collapses_runs_of_separators() {
    ASSERT(search::fold_and_split("  ...a??  b  ") ==
           (std::vector<std::string>{"a", "b"}));
    ASSERT(search::fold_and_split("").empty());
    ASSERT(search::fold_and_split("!!! ???").empty());
}

static void test_folds_latin_diacritics_to_ascii() {
    // Latin-1 Supplement, with ligature expansion.
    ASSERT(search::fold_and_split("Café NAÏVE Zürich Ångström Æther ß") ==
           (std::vector<std::string>{"cafe", "naive", "zurich", "angstrom",
                                     "aether", "ss"}));
    // Latin Extended-A.
    ASSERT(search::fold_and_split("Łódź Škoda Erdoğan") ==
           (std::vector<std::string>{"lodz", "skoda", "erdogan"}));
}

static void test_combining_marks_do_not_split_tokens() {
    // "cafe" + U+0301 COMBINING ACUTE ACCENT must fold to the same token as
    // the precomposed "café".
    const std::string decomposed = "cafe\xCC\x81";
    ASSERT(search::fold_and_split(decomposed) ==
           (std::vector<std::string>{"cafe"}));
    // Soft hyphen (U+00AD) inside a word disappears without splitting it.
    ASSERT(search::fold_and_split("co\xC2\xADoperate") ==
           (std::vector<std::string>{"cooperate"}));
}

static void test_symbols_and_emoji_are_separators() {
    // Em dash, curly quotes and an emoji all split.
    ASSERT(search::fold_and_split("alpha\xE2\x80\x94" "beta \xF0\x9F\x94\x8D gamma") ==
           (std::vector<std::string>{"alpha", "beta", "gamma"}));
}

static void test_invalid_utf8_does_not_crash() {
    // Lone continuation byte and a truncated 3-byte sequence. (The literals
    // are split so that a following letter is not eaten by the hex escape.)
    ASSERT(search::fold_and_split("ok\x80" "ay") ==
           (std::vector<std::string>{"ok", "ay"}));
    ASSERT(search::fold_and_split("trunc\xE2\x80") ==
           (std::vector<std::string>{"trunc"}));
}

// ---------------------------------------------------------------------------
// Stage 3: stopwords
// ---------------------------------------------------------------------------

static void test_stopwords_are_dropped() {
    expect_tokens("the and of a an is are was were", {});
    ASSERT(search::is_stopword("the"));
    ASSERT(search::is_stopword("themselves"));
    ASSERT(!search::is_stopword("theme"));
    ASSERT(!search::is_stopword("compiler"));
}

static void test_stopword_check_precedes_stemming() {
    // "having" is a stopword; if we stemmed first it would become "have" and
    // still be dropped, but "this" -> "thi" would survive. Check the order.
    expect_tokens("this having", {});
}

static void test_content_words_survive_next_to_stopwords() {
    expect_tokens("a fox and the dog", {"fox", "dog"});
}

// ---------------------------------------------------------------------------
// Stage 4: Porter stemming
// ---------------------------------------------------------------------------

static void test_porter_short_words_untouched() {
    expect_stem("a", "a");
    expect_stem("at", "at");
    expect_stem("be", "be");
    expect_stem("sky", "sky");   // step 1c needs a vowel in the stem
}

static void test_porter_step1a_plurals() {
    expect_stem("caresses", "caress");
    expect_stem("ponies", "poni");
    expect_stem("ties", "ti");
    expect_stem("caress", "caress");
    expect_stem("cats", "cat");
}

static void test_porter_step1b_past_forms() {
    expect_stem("feed", "feed");      // m(fe) = 0, -eed kept
    expect_stem("agreed", "agre");    // -eed -> -ee, then step 5 drops one e
    expect_stem("plastered", "plaster");
    expect_stem("motoring", "motor");
    expect_stem("sing", "sing");      // no vowel before -ing to strip
    expect_stem("conflated", "conflat");
    expect_stem("troubling", "troubl");
    expect_stem("hopping", "hop");    // doubled consonant reduced
    expect_stem("falling", "fall");   // -ll is kept
    expect_stem("hissing", "hiss");
    expect_stem("filing", "file");    // cvc restores the silent e
}

static void test_porter_step1c_terminal_y() {
    expect_stem("happy", "happi");
    expect_stem("sky", "sky");
}

static void test_porter_multi_step_words() {
    expect_stem("relational", "relat");
    expect_stem("national", "nation");
    expect_stem("generalization", "gener");
    expect_stem("connection", "connect");
    // step 2 (-fulness -> -ful) then step 3 (-ful -> "") both fire.
    expect_stem("hopefulness", "hope");
    expect_stem("hopeful", "hope");
    expect_stem("goodness", "good");
    expect_stem("argument", "argument");  // m(argu) = 1, -ment kept
    expect_stem("replacement", "replac");
    expect_stem("adjustable", "adjust");
    expect_stem("controlling", "control");
    expect_stem("roll", "roll");
}

static void test_porter_conflates_word_family() {
    const std::vector<std::string> family = {
        "connect", "connects", "connected", "connecting", "connection",
        "connections",
    };
    for (const auto& w : family) {
        expect_stem(w, "connect");
    }
}

static void test_non_ascii_tokens_pass_through_stemming() {
    // Tokens that are not pure a-z are returned unchanged: digits stay, and a
    // script we do not fold to ASCII is indexed verbatim.
    expect_stem("bm25", "bm25");
    expect_stem("42", "42");
    const std::string cyrillic = "\xD0\xBF\xD0\xBE\xD0\xB8\xD1\x81\xD0\xBA";  // поиск
    expect_stem(cyrillic, cyrillic);
}

static void test_cyrillic_and_greek_lowercased() {
    // ПОИСК -> поиск
    ASSERT(search::fold_and_split("\xD0\x9F\xD0\x9E\xD0\x98\xD0\xA1\xD0\x9A") ==
           (std::vector<std::string>{
               "\xD0\xBF\xD0\xBE\xD0\xB8\xD1\x81\xD0\xBA"}));
    // ΛΟΓΟΣ -> λογοσ (final-sigma normalisation is not attempted)
    ASSERT(search::fold_and_split("\xCE\x9B\xCE\x9F\xCE\x93\xCE\x9F\xCE\xA3") ==
           (std::vector<std::string>{
               "\xCE\xBB\xCE\xBF\xCE\xB3\xCE\xBF\xCF\x83"}));
}

// ---------------------------------------------------------------------------
// Whole pipeline
// ---------------------------------------------------------------------------

static void test_pipeline_end_to_end() {
    expect_tokens("The Quick, Brown Foxes Jumped Over 2 Lazy Dogs!",
                  {"quick", "brown", "fox", "jump", "2", "lazi", "dog"});
}

static void test_query_and_document_forms_agree() {
    // The point of a shared tokenizer: a query written differently from the
    // document still produces the same terms.
    ASSERT(search::tokenize("Running the compilers") ==
           search::tokenize("RUNS a COMPILER"));
    ASSERT(search::tokenize("Café") == search::tokenize("cafes"));
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "\n=== Tokenizer Unit Tests ===\n";

    run_test("acceptance_running_the_compilers", test_acceptance_running_the_compilers);

    run_test("lowercases_ascii", test_lowercases_ascii);
    run_test("splits_on_non_alphanumeric", test_splits_on_non_alphanumeric);
    run_test("keeps_digits_and_alphanumeric_runs", test_keeps_digits_and_alphanumeric_runs);
    run_test("collapses_runs_of_separators", test_collapses_runs_of_separators);
    run_test("folds_latin_diacritics_to_ascii", test_folds_latin_diacritics_to_ascii);
    run_test("combining_marks_do_not_split_tokens", test_combining_marks_do_not_split_tokens);
    run_test("symbols_and_emoji_are_separators", test_symbols_and_emoji_are_separators);
    run_test("invalid_utf8_does_not_crash", test_invalid_utf8_does_not_crash);

    run_test("stopwords_are_dropped", test_stopwords_are_dropped);
    run_test("stopword_check_precedes_stemming", test_stopword_check_precedes_stemming);
    run_test("content_words_survive_next_to_stopwords", test_content_words_survive_next_to_stopwords);

    run_test("porter_short_words_untouched", test_porter_short_words_untouched);
    run_test("porter_step1a_plurals", test_porter_step1a_plurals);
    run_test("porter_step1b_past_forms", test_porter_step1b_past_forms);
    run_test("porter_step1c_terminal_y", test_porter_step1c_terminal_y);
    run_test("porter_multi_step_words", test_porter_multi_step_words);
    run_test("porter_conflates_word_family", test_porter_conflates_word_family);
    run_test("non_ascii_tokens_pass_through_stemming", test_non_ascii_tokens_pass_through_stemming);
    run_test("cyrillic_and_greek_lowercased", test_cyrillic_and_greek_lowercased);

    run_test("pipeline_end_to_end", test_pipeline_end_to_end);
    run_test("query_and_document_forms_agree", test_query_and_document_forms_agree);

    std::cout << "\n" << g_tests_passed << "/" << g_tests_run << " tests passed.\n\n";
    return (g_tests_passed == g_tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
