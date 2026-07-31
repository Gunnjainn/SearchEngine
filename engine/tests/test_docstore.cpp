// ---------------------------------------------------------------------------
// Unit tests for the disk-backed doc store and snippet extraction.
//
// The two headline tests are:
//   doc_text_and_meta_correct_after_load  — the acceptance criterion
//   ram_stays_bounded_as_text_grows       — the other half of it: the offset
//       table must scale with the document count, not the corpus size
// ---------------------------------------------------------------------------

#include "doc_store.h"
#include "engine.h"
#include "index_io.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace io = search::io;

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

// ── Fixtures ─────────────────────────────────────────────────────────────────

// Text chosen to break a naive store: a quote that must survive JSON escaping,
// an escaped newline, multi-byte UTF-8, and a large doc_id gap.
static std::string write_corpus(const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) throw std::runtime_error("cannot write " + path);
    out << R"({"doc_id": 30, "title": "Compiler notes", "url": "http://e.test/30", "text": "Running the compilers requires patience."})" << "\n";
    out << R"({"doc_id": 10, "title": "Quoted \"title\"", "url": "http://e.test/10", "text": "A compiler compiles code.\nCompilers are tools."})" << "\n";
    out << R"({"doc_id": 900000, "title": "Café index", "url": "http://e.test/900000", "text": "Indexes make databases fast. Café naïve façade."})" << "\n";
    out.close();
    return path;
}

static const char* kExpectedText30 = "Running the compilers requires patience.";
static const char* kExpectedText10 = "A compiler compiles code.\nCompilers are tools.";
static const char* kExpectedText900k = "Indexes make databases fast. Caf\xC3\xA9 na\xC3\xAFve fa\xC3\xA7" "ade.";

static void check_all_documents(const Engine& e) {
    ASSERT(e.doc_text(30) == kExpectedText30);
    ASSERT(e.doc_text(10) == kExpectedText10);
    ASSERT(e.doc_text(900000) == kExpectedText900k);

    ASSERT(e.doc_meta(30).title == "Compiler notes");
    ASSERT(e.doc_meta(30).url == "http://e.test/30");
    ASSERT(e.doc_meta(10).title == "Quoted \"title\"");
    ASSERT(e.doc_meta(10).url == "http://e.test/10");
    ASSERT(e.doc_meta(900000).title == "Caf\xC3\xA9 index");
    ASSERT(e.doc_meta(900000).url == "http://e.test/900000");
}

// ---------------------------------------------------------------------------
// Reading through the JSONL backing (straight after build)
// ---------------------------------------------------------------------------

static void test_reads_through_jsonl_after_build() {
    Engine e;
    e.build_from_jsonl(write_corpus("test_ds_build.jsonl"));

    ASSERT(e.num_docs() == 3);
    check_all_documents(e);

    // No text was copied into memory: the store still points at the corpus.
    ASSERT(!e.doc_store().is_packed());
    ASSERT(e.doc_store().backing_path() == "test_ds_build.jsonl");
}

static void test_unknown_doc_id_is_empty() {
    Engine e;
    e.build_from_jsonl(write_corpus("test_ds_unknown.jsonl"));

    ASSERT(e.doc_text(999).empty());
    ASSERT(e.doc_meta(999).title.empty());
    ASSERT(e.doc_meta(999).url.empty());
    ASSERT(e.doc_length(999) == 0);
}

static void test_offsets_survive_crlf_line_endings() {
    // A CRLF corpus: byte offsets must count the \r, which is why the JSONL is
    // opened in binary mode rather than text mode.
    const std::string path = "test_ds_crlf.jsonl";
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({"doc_id": 1, "title": "One", "url": "u1", "text": "first document"})" << "\r\n";
        out << R"({"doc_id": 2, "title": "Two", "url": "u2", "text": "second document"})" << "\r\n";
        out << R"({"doc_id": 3, "title": "Three", "url": "u3", "text": "third document"})" << "\r\n";
    }

    Engine e;
    e.build_from_jsonl(path);
    ASSERT(e.num_docs() == 3);
    ASSERT(e.doc_text(1) == "first document");
    ASSERT(e.doc_text(2) == "second document");
    ASSERT(e.doc_text(3) == "third document");
    ASSERT(e.doc_meta(3).title == "Three");
}

// ---------------------------------------------------------------------------
// Reading through the packed backing (after save / load)
// ---------------------------------------------------------------------------

static void test_save_repoints_store_at_packed_copy() {
    const std::string dir  = "test_ds_repoint";
    const std::string json = "test_ds_repoint.jsonl";
    fs::remove_all(dir);

    Engine e;
    e.build_from_jsonl(write_corpus(json));
    ASSERT(!e.doc_store().is_packed());

    ASSERT(e.save(dir));
    ASSERT(e.doc_store().is_packed());

    // The saved index is self-contained, so deleting the corpus changes nothing.
    fs::remove(json);
    ASSERT(!fs::exists(json));
    check_all_documents(e);
}

// The acceptance criterion.
static void test_doc_text_and_meta_correct_after_load() {
    const std::string dir  = "test_ds_afterload";
    const std::string json = "test_ds_afterload.jsonl";
    fs::remove_all(dir);

    {
        Engine built;
        built.build_from_jsonl(write_corpus(json));
        ASSERT(built.save(dir));
    }

    // A fresh process would not have the corpus.
    fs::remove(json);
    ASSERT(!fs::exists(json));

    Engine loaded;
    ASSERT(loaded.load(dir));
    ASSERT(loaded.num_docs() == 3);
    ASSERT(loaded.doc_store().is_packed());

    check_all_documents(loaded);

    // Token counts come back too, so BM25 scores the same way.
    ASSERT(loaded.doc_length(30) == 6);
    ASSERT(loaded.doc_length(10) > 0);
    ASSERT(loaded.doc_length(900000) > 0);
}

static void test_docs_idx_is_part_of_the_index() {
    const std::string dir = "test_ds_files";
    fs::remove_all(dir);

    Engine e;
    e.build_from_jsonl(write_corpus("test_ds_files.jsonl"));
    ASSERT(e.save(dir));

    for (const char* name : {io::kMetaFile, io::kDocsIdxFile, io::kDocsFile, io::kTermsFile}) {
        ASSERT(fs::exists(fs::path(dir) / name));
    }
    // 16 bytes per document: doc_id, token count, offset.
    ASSERT(fs::file_size(fs::path(dir) / io::kDocsIdxFile) == 3 * 16);
    ASSERT(io::index_size_bytes(dir) > 0);
}

static void test_load_rejects_offset_past_end_of_docs() {
    const std::string dir = "test_ds_badoffset";
    fs::remove_all(dir);

    Engine built;
    built.build_from_jsonl(write_corpus("test_ds_badoffset.jsonl"));
    ASSERT(built.save(dir));

    // Truncating docs.bin leaves the recorded offsets pointing past its end.
    const fs::path docs = fs::path(dir) / io::kDocsFile;
    const auto full = fs::file_size(docs);
    {
        std::ifstream in(docs, std::ios::binary);
        std::string head(static_cast<std::size_t>(full / 4), '\0');
        in.read(&head[0], static_cast<std::streamsize>(head.size()));
        std::ofstream out(docs, std::ios::binary | std::ios::trunc);
        out.write(head.data(), static_cast<std::streamsize>(head.size()));
    }

    Engine e;
    ASSERT(!e.load(dir));
    ASSERT(e.num_docs() == 0);
}

// ---------------------------------------------------------------------------
// Bounded memory
// ---------------------------------------------------------------------------

// The other half of the acceptance criterion: offsets, not all text in RAM.
static void test_ram_stays_bounded_as_text_grows() {
    const std::string json = "test_ds_big.jsonl";
    const std::string dir  = "test_ds_big_index";
    fs::remove_all(dir);

    // 40 documents of ~100 KB each: about 4 MB of text.
    constexpr int kDocs = 40;
    constexpr int kBodyBytes = 100 * 1024;
    {
        std::ofstream out(json, std::ios::binary);
        for (int i = 0; i < kDocs; ++i) {
            const std::string body = "compiler " + std::string(kBodyBytes, 'x');
            out << R"({"doc_id": )" << (i + 1) << R"(, "title": "Doc )" << (i + 1)
                << R"(", "url": "u", "text": ")" << body << R"("})" << "\n";
        }
    }
    const auto corpus_bytes = fs::file_size(json);
    ASSERT(corpus_bytes > 3u * 1024u * 1024u);

    Engine e;
    e.build_from_jsonl(json);
    ASSERT(e.num_docs() == kDocs);
    ASSERT(e.save(dir));

    Engine loaded;
    ASSERT(loaded.load(dir));
    ASSERT(loaded.num_docs() == kDocs);

    // The offset table must be a tiny fraction of the text it addresses. A
    // store that kept text in RAM would be at least as large as the corpus.
    const std::size_t ram = loaded.doc_store().approx_ram_bytes();
    ASSERT(ram < 8 * 1024);                    // kilobytes, for megabytes of text
    ASSERT(ram < corpus_bytes / 100);

    // And the text is still fully readable on demand.
    const std::string text = loaded.doc_text(7);
    ASSERT(text.size() > static_cast<std::size_t>(kBodyBytes));
    ASSERT(text.rfind("compiler ", 0) == 0);
    ASSERT(loaded.doc_meta(7).title == "Doc 7");
}

static void test_ram_scales_with_doc_count_not_text_size() {
    // Same document count, ten times the text: RAM must not follow the text.
    const auto build_and_measure = [](const std::string& json, int body_bytes) {
        {
            std::ofstream out(json, std::ios::binary);
            for (int i = 0; i < 20; ++i) {
                out << R"({"doc_id": )" << (i + 1) << R"(, "title": "T", "url": "u", "text": ")"
                    << std::string(static_cast<std::size_t>(body_bytes), 'y') << R"("})" << "\n";
            }
        }
        Engine e;
        e.build_from_jsonl(json);
        return e.doc_store().approx_ram_bytes();
    };

    const std::size_t small = build_and_measure("test_ds_scale_small.jsonl", 1000);
    const std::size_t large = build_and_measure("test_ds_scale_large.jsonl", 100000);
    ASSERT(small == large);
}

// ---------------------------------------------------------------------------
// Snippets
// ---------------------------------------------------------------------------

static void test_snippet_passes_short_text_through() {
    ASSERT(search::make_snippet("short", 150) == "short");
    ASSERT(search::make_snippet("", 150).empty());
    // Exactly at the limit is not truncated.
    const std::string exact(150, 'a');
    ASSERT(search::make_snippet(exact, 150) == exact);
}

static void test_snippet_truncates_with_ellipsis() {
    const std::string long_text(200, 'a');
    const std::string got = search::make_snippet(long_text, 150);
    ASSERT(got.size() == 153);
    ASSERT(got.compare(150, 3, "...") == 0);
}

static void test_snippet_never_splits_a_utf8_character() {
    // "é" is two bytes. Cutting at 5 would land inside it, so the snippet must
    // back off to 4 bytes. A half character would make the JSON response
    // invalid UTF-8 and nlohmann's dump() throws on that.
    const std::string text = "abcd\xC3\xA9xxxxxxxxxx";
    const std::string got = search::make_snippet(text, 5);
    ASSERT(got == "abcd...");

    // Cutting exactly on the boundary keeps the whole character.
    const std::string got6 = search::make_snippet(text, 6);
    ASSERT(got6 == "abcd\xC3\xA9...");

    // A three-byte character cut at any interior offset behaves the same way.
    // (Literal split so "cd" is not swallowed by the \xAC escape.)
    const std::string euro = "ab\xE2\x82\xAC" "cd";
    ASSERT(search::make_snippet(euro, 3) == "ab...");
    ASSERT(search::make_snippet(euro, 4) == "ab...");
    ASSERT(search::make_snippet(euro, 5) == "ab\xE2\x82\xAC...");
}

static void test_search_snippets_come_from_the_store() {
    const std::string dir = "test_ds_snippets";
    fs::remove_all(dir);

    Engine e;
    e.build_from_jsonl(write_corpus("test_ds_snippets.jsonl"));
    ASSERT(e.save(dir));
    fs::remove("test_ds_snippets.jsonl");

    const auto hits = e.search("compile", 10);
    ASSERT(hits.size() == 2);
    for (const Result& r : hits) {
        ASSERT(!r.snippet.empty());
        ASSERT(r.snippet == search::make_snippet(e.doc_text(r.doc_id), 150));
    }
}

static void test_long_document_snippet_is_capped() {
    const std::string json = "test_ds_longdoc.jsonl";
    {
        std::ofstream out(json, std::ios::binary);
        out << R"({"doc_id": 1, "title": "Long", "url": "u", "text": "compiler )"
            << std::string(5000, 'z') << R"("})" << "\n";
    }

    Engine e;
    e.build_from_jsonl(json);
    const auto hits = e.search("compile", 10);
    ASSERT(hits.size() == 1);
    ASSERT(hits[0].snippet.size() <= 153);
    ASSERT(hits[0].snippet.size() >= 150);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "\n=== Doc Store Unit Tests ===\n";

    run_test("reads_through_jsonl_after_build", test_reads_through_jsonl_after_build);
    run_test("unknown_doc_id_is_empty", test_unknown_doc_id_is_empty);
    run_test("offsets_survive_crlf_line_endings", test_offsets_survive_crlf_line_endings);

    run_test("save_repoints_store_at_packed_copy", test_save_repoints_store_at_packed_copy);
    run_test("doc_text_and_meta_correct_after_load", test_doc_text_and_meta_correct_after_load);
    run_test("docs_idx_is_part_of_the_index", test_docs_idx_is_part_of_the_index);
    run_test("load_rejects_offset_past_end_of_docs", test_load_rejects_offset_past_end_of_docs);

    run_test("ram_stays_bounded_as_text_grows", test_ram_stays_bounded_as_text_grows);
    run_test("ram_scales_with_doc_count_not_text_size", test_ram_scales_with_doc_count_not_text_size);

    run_test("snippet_passes_short_text_through", test_snippet_passes_short_text_through);
    run_test("snippet_truncates_with_ellipsis", test_snippet_truncates_with_ellipsis);
    run_test("snippet_never_splits_a_utf8_character", test_snippet_never_splits_a_utf8_character);
    run_test("search_snippets_come_from_the_store", test_search_snippets_come_from_the_store);
    run_test("long_document_snippet_is_capped", test_long_document_snippet_is_capped);

    std::cout << "\n" << g_tests_passed << "/" << g_tests_run << " tests passed.\n\n";
    return (g_tests_passed == g_tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
