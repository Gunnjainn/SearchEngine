// ---------------------------------------------------------------------------
// Unit tests for the on-disk index format: the binary primitives in
// search::io, and Engine::save / Engine::load.
//
// The headline test is round_trip_query_results_are_identical, which is the
// task's acceptance criterion: build -> save -> delete the JSONL -> load into a
// fresh Engine -> identical query results.
// ---------------------------------------------------------------------------

#include "engine.h"
#include "index_io.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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

// A corpus with out-of-order doc_ids, a large doc_id gap (to exercise multi-byte
// varints), repeated terms, and non-ASCII text that must survive byte-for-byte.
static std::string write_corpus(const std::string& path) {
    std::ofstream out(path);
    if (!out.is_open()) throw std::runtime_error("cannot write " + path);
    out << R"({"doc_id": 30, "title": "Compiler notes", "url": "http://e.test/30", "text": "Running the compilers requires patience."})" << "\n";
    out << R"({"doc_id": 10, "title": "Compilers", "url": "http://e.test/10", "text": "A compiler compiles code. Compilers are tools."})" << "\n";
    out << R"({"doc_id": 900000, "title": "Café index", "url": "http://e.test/900000", "text": "Indexes make databases fast. Café naïve."})" << "\n";
    out.close();
    return path;
}

static const std::vector<std::string>& probe_queries() {
    static const std::vector<std::string> queries = {
        "compile", "compilers", "database index", "cafe", "patience",
        "the and of", "", "zzzznotaterm", "compile database",
    };
    return queries;
}

static void expect_same_results(const Engine& a, const Engine& b) {
    for (const std::string& q : probe_queries()) {
        const auto ra = a.search(q, 10);
        const auto rb = b.search(q, 10);
        if (ra.size() != rb.size()) {
            throw std::runtime_error("result count differs for query \"" + q + "\": "
                + std::to_string(ra.size()) + " vs " + std::to_string(rb.size()));
        }
        for (std::size_t i = 0; i < ra.size(); ++i) {
            if (ra[i].doc_id != rb[i].doc_id) {
                throw std::runtime_error("doc_id differs for \"" + q + "\" at " + std::to_string(i));
            }
            // Scores derive from integer counts, so they must match bit for bit.
            if (ra[i].score != rb[i].score) {
                throw std::runtime_error("score differs for \"" + q + "\" at " + std::to_string(i));
            }
            if (ra[i].snippet != rb[i].snippet) {
                throw std::runtime_error("snippet differs for \"" + q + "\" at " + std::to_string(i));
            }
        }
    }
}

static std::string read_whole_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

// ---------------------------------------------------------------------------
// Binary primitives
// ---------------------------------------------------------------------------

static void test_varint_round_trip() {
    const std::vector<std::uint32_t> values = {
        0, 1, 2, 126, 127, 128, 129, 255, 256, 16383, 16384, 16385,
        2097151, 2097152, 268435455, 268435456,
        std::numeric_limits<std::uint32_t>::max() - 1,
        std::numeric_limits<std::uint32_t>::max(),
    };

    for (std::uint32_t v : values) {
        std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
        io::write_varint(ss, v);
        std::uint32_t got = 0;
        ASSERT(io::read_varint(ss, got));
        ASSERT(got == v);
        ASSERT(io::at_eof(ss));  // consumed exactly the bytes written
    }
}

static void test_varint_is_compact_for_small_values() {
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    io::write_varint(ss, 1);
    ASSERT(ss.str().size() == 1);

    std::stringstream big(std::ios::in | std::ios::out | std::ios::binary);
    io::write_varint(big, std::numeric_limits<std::uint32_t>::max());
    ASSERT(big.str().size() == 5);
}

static void test_varint_rejects_malformed() {
    // Six continuation bytes: no terminator within the 5 a u32 can need.
    std::stringstream ss(std::string("\x80\x80\x80\x80\x80\x80", 6),
                         std::ios::in | std::ios::binary);
    std::uint32_t v = 0;
    ASSERT(!io::read_varint(ss, v));

    // Truncated: continuation bit set, then nothing.
    std::stringstream trunc(std::string("\x80", 1), std::ios::in | std::ios::binary);
    ASSERT(!io::read_varint(trunc, v));

    // Fifth group carries more than the 4 bits a u32 has left.
    std::stringstream wide(std::string("\x80\x80\x80\x80\x7F", 5),
                           std::ios::in | std::ios::binary);
    ASSERT(!io::read_varint(wide, v));
}

static void test_scalar_round_trip() {
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    io::write_u32(ss, 0xDEADBEEFu);
    io::write_u64(ss, 0x0123456789ABCDEFull);
    io::write_i32(ss, -42);
    io::write_i32(ss, std::numeric_limits<std::int32_t>::min());

    std::uint32_t a = 0;
    std::uint64_t b = 0;
    std::int32_t  c = 0, d = 0;
    ASSERT(io::read_u32(ss, a) && a == 0xDEADBEEFu);
    ASSERT(io::read_u64(ss, b) && b == 0x0123456789ABCDEFull);
    ASSERT(io::read_i32(ss, c) && c == -42);
    ASSERT(io::read_i32(ss, d) && d == std::numeric_limits<std::int32_t>::min());
    ASSERT(io::at_eof(ss));
}

static void test_scalars_are_little_endian_on_disk() {
    // The format is fixed, not host-dependent: low byte first.
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    io::write_u32(ss, 0x04030201u);
    const std::string bytes = ss.str();
    ASSERT(bytes.size() == 4);
    ASSERT(static_cast<unsigned char>(bytes[0]) == 0x01);
    ASSERT(static_cast<unsigned char>(bytes[1]) == 0x02);
    ASSERT(static_cast<unsigned char>(bytes[2]) == 0x03);
    ASSERT(static_cast<unsigned char>(bytes[3]) == 0x04);
}

static void test_string_round_trip() {
    const std::vector<std::string> values = {
        "", "a", "compil", "Caf\xC3\xA9", "line\nbreak", std::string("nul\0inside", 10),
        std::string(70000, 'x'),
    };
    for (const std::string& v : values) {
        std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
        io::write_string(ss, v);
        std::string got;
        ASSERT(io::read_string(ss, got));
        ASSERT(got == v);
        ASSERT(io::at_eof(ss));
    }
}

static void test_string_rejects_absurd_length() {
    // Length prefix far beyond the cap: must fail, not try to allocate it.
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    io::write_u32(ss, 0xFFFFFFFFu);
    std::string got;
    ASSERT(!io::read_string(ss, got));
}

static void test_string_rejects_truncation() {
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    io::write_u32(ss, 10);      // claims 10 bytes
    ss.write("abc", 3);         // provides 3
    std::string got;
    ASSERT(!io::read_string(ss, got));
}

// ---------------------------------------------------------------------------
// save / load round trip
// ---------------------------------------------------------------------------

static void test_save_creates_the_index_files() {
    const std::string dir = "test_persist_files";
    fs::remove_all(dir);

    Engine e;
    e.build_from_jsonl(write_corpus("test_persist_a.jsonl"));
    ASSERT(e.save(dir));

    ASSERT(fs::exists(fs::path(dir) / io::kMetaFile));
    ASSERT(fs::exists(fs::path(dir) / io::kDocsIdxFile));
    ASSERT(fs::exists(fs::path(dir) / io::kDocsFile));
    ASSERT(fs::exists(fs::path(dir) / io::kTermsFile));

    const std::uint64_t bytes = io::index_size_bytes(dir);
    ASSERT(bytes > 0);
    // Sanity: the index is at least the corpus text and at most wildly larger.
    ASSERT(bytes > 100);
    ASSERT(bytes < 1024 * 1024);
}

static void test_save_creates_missing_directories() {
    const std::string dir = "test_persist_nested/a/b/c";
    fs::remove_all("test_persist_nested");

    Engine e;
    e.build_from_jsonl(write_corpus("test_persist_b.jsonl"));
    ASSERT(e.save(dir));
    ASSERT(fs::exists(fs::path(dir) / io::kMetaFile));
}

static void test_load_restores_index_structure() {
    const std::string dir = "test_persist_structure";
    fs::remove_all(dir);

    Engine built;
    built.build_from_jsonl(write_corpus("test_persist_c.jsonl"));
    ASSERT(built.save(dir));

    Engine loaded;
    ASSERT(loaded.load(dir));

    ASSERT(loaded.num_docs() == built.num_docs());
    ASSERT(loaded.num_terms() == built.num_terms());
    ASSERT(loaded.avg_doc_length() == built.avg_doc_length());

    for (int doc_id : {10, 30, 900000}) {
        ASSERT(loaded.doc_length(doc_id) == built.doc_length(doc_id));
        ASSERT(loaded.doc_text(doc_id) == built.doc_text(doc_id));
        ASSERT(loaded.doc_meta(doc_id).title == built.doc_meta(doc_id).title);
        ASSERT(loaded.doc_meta(doc_id).url == built.doc_meta(doc_id).url);
    }

    // Postings must come back with the same order and term frequencies.
    for (const char* term : {"compil", "databas", "index", "cafe", "patienc", "tool"}) {
        const auto& a = built.postings(term);
        const auto& b = loaded.postings(term);
        ASSERT(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            ASSERT(a[i].doc_id == b[i].doc_id);
            ASSERT(a[i].term_freq == b[i].term_freq);
        }
    }
}

// The acceptance criterion.
static void test_round_trip_query_results_are_identical() {
    const std::string dir  = "test_persist_roundtrip";
    const std::string json = "test_persist_roundtrip.jsonl";
    fs::remove_all(dir);

    Engine built;
    built.build_from_jsonl(write_corpus(json));
    ASSERT(built.num_docs() == 3);
    ASSERT(built.save(dir));

    // A fresh process would not have the JSONL. Remove it so that a load()
    // which secretly depended on it cannot pass.
    fs::remove(json);
    ASSERT(!fs::exists(json));

    Engine loaded;
    ASSERT(loaded.load(dir));
    ASSERT(loaded.num_docs() == 3);

    expect_same_results(built, loaded);

    // And the loaded index really answers queries, not just empty lists.
    const auto hits = loaded.search("compile", 10);
    ASSERT(hits.size() == 2);
    ASSERT(hits[0].doc_id == 10);
    ASSERT(!hits[0].snippet.empty());
}

static void test_save_is_byte_deterministic() {
    const std::string a = "test_persist_det_a";
    const std::string b = "test_persist_det_b";
    fs::remove_all(a);
    fs::remove_all(b);

    Engine built;
    built.build_from_jsonl(write_corpus("test_persist_d.jsonl"));
    ASSERT(built.save(a));

    // save -> load -> save must reproduce the same bytes. Terms are written in
    // sorted order precisely so that hash iteration order cannot leak in.
    Engine loaded;
    ASSERT(loaded.load(a));
    ASSERT(loaded.save(b));

    for (const char* name : {io::kMetaFile, io::kDocsIdxFile, io::kDocsFile, io::kTermsFile}) {
        ASSERT(read_whole_file(fs::path(a) / name) == read_whole_file(fs::path(b) / name));
    }
}

static void test_load_replaces_existing_index() {
    const std::string dir = "test_persist_replace";
    fs::remove_all(dir);

    Engine small;
    {
        const std::string p = "test_persist_small.jsonl";
        std::ofstream out(p);
        out << R"({"doc_id": 1, "title": "Only", "url": "u", "text": "database"})" << "\n";
        out.close();
        small.build_from_jsonl(p);
    }
    ASSERT(small.save(dir));

    // An engine that already holds a different corpus must end up with exactly
    // the loaded one, not a merge of the two.
    Engine target;
    target.build_from_jsonl(write_corpus("test_persist_e.jsonl"));
    ASSERT(target.num_docs() == 3);

    ASSERT(target.load(dir));
    ASSERT(target.num_docs() == 1);
    ASSERT(target.postings("compil").empty());
    ASSERT(target.postings("databas").size() == 1);
    ASSERT(target.doc_length(30) == 0);  // gone
}

// ---------------------------------------------------------------------------
// Failure handling
// ---------------------------------------------------------------------------

static void test_load_missing_directory_fails() {
    Engine e;
    ASSERT(!e.load("test_persist_does_not_exist"));
    ASSERT(e.num_docs() == 0);
}

static void test_failed_load_leaves_engine_intact() {
    const std::string dir = "test_persist_intact";
    fs::remove_all(dir);

    Engine e;
    e.build_from_jsonl(write_corpus("test_persist_f.jsonl"));
    const auto before = e.search("compile", 10);
    ASSERT(before.size() == 2);

    ASSERT(!e.load("test_persist_no_such_dir"));

    // Still fully usable after the failed load.
    const auto after = e.search("compile", 10);
    ASSERT(after.size() == before.size());
    ASSERT(after[0].doc_id == before[0].doc_id);
    ASSERT(e.num_docs() == 3);
}

static void test_load_rejects_bad_magic() {
    const std::string dir = "test_persist_badmagic";
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::ofstream out(fs::path(dir) / io::kMetaFile, std::ios::binary);
    out << "NOPE";
    io::write_u32(out, io::kVersion);
    out.close();

    Engine e;
    ASSERT(!e.load(dir));
}

static void test_load_rejects_version_mismatch() {
    const std::string dir = "test_persist_badversion";
    fs::remove_all(dir);

    Engine built;
    built.build_from_jsonl(write_corpus("test_persist_g.jsonl"));
    ASSERT(built.save(dir));

    // Bump the version field in place; everything else stays valid.
    const fs::path meta = fs::path(dir) / io::kMetaFile;
    std::string bytes = read_whole_file(meta);
    ASSERT(bytes.size() > 8);
    {
        std::ofstream out(meta, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), 4);              // magic
        io::write_u32(out, io::kVersion + 99);   // wrong version
        out.write(bytes.data() + 8, static_cast<std::streamsize>(bytes.size() - 8));
    }

    Engine e;
    ASSERT(!e.load(dir));
}

static void test_load_rejects_truncated_files() {
    const std::string dir = "test_persist_truncated";

    for (const char* victim : {io::kDocsIdxFile, io::kDocsFile, io::kTermsFile}) {
        fs::remove_all(dir);
        Engine built;
        built.build_from_jsonl(write_corpus("test_persist_h.jsonl"));
        ASSERT(built.save(dir));

        const fs::path target = fs::path(dir) / victim;
        std::string bytes = read_whole_file(target);
        ASSERT(bytes.size() > 12);
        {
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size() / 2));
        }

        Engine e;
        ASSERT(!e.load(dir));   // must fail, not crash or half-load
        ASSERT(e.num_docs() == 0);
    }
}

static void test_load_rejects_trailing_garbage() {
    const std::string dir = "test_persist_garbage";
    fs::remove_all(dir);

    Engine built;
    built.build_from_jsonl(write_corpus("test_persist_i.jsonl"));
    ASSERT(built.save(dir));

    {
        std::ofstream out(fs::path(dir) / io::kTermsFile, std::ios::binary | std::ios::app);
        out << "extra bytes that no reader expects";
    }

    Engine e;
    ASSERT(!e.load(dir));
}

static void test_index_size_of_incomplete_index_is_zero() {
    const std::string dir = "test_persist_incomplete";
    fs::remove_all(dir);

    Engine built;
    built.build_from_jsonl(write_corpus("test_persist_j.jsonl"));
    ASSERT(built.save(dir));
    ASSERT(io::index_size_bytes(dir) > 0);

    fs::remove(fs::path(dir) / io::kTermsFile);
    ASSERT(io::index_size_bytes(dir) == 0);
}

static void test_empty_index_round_trips() {
    const std::string dir = "test_persist_empty";
    fs::remove_all(dir);

    Engine empty;
    ASSERT(empty.save(dir));

    Engine loaded;
    ASSERT(loaded.load(dir));
    ASSERT(loaded.num_docs() == 0);
    ASSERT(loaded.num_terms() == 0);
    ASSERT(loaded.avg_doc_length() == 0.0);
    ASSERT(loaded.search("compile", 10).empty());
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "\n=== Persistence Unit Tests ===\n";

    run_test("varint_round_trip", test_varint_round_trip);
    run_test("varint_is_compact_for_small_values", test_varint_is_compact_for_small_values);
    run_test("varint_rejects_malformed", test_varint_rejects_malformed);
    run_test("scalar_round_trip", test_scalar_round_trip);
    run_test("scalars_are_little_endian_on_disk", test_scalars_are_little_endian_on_disk);
    run_test("string_round_trip", test_string_round_trip);
    run_test("string_rejects_absurd_length", test_string_rejects_absurd_length);
    run_test("string_rejects_truncation", test_string_rejects_truncation);

    run_test("save_creates_the_index_files", test_save_creates_the_index_files);
    run_test("save_creates_missing_directories", test_save_creates_missing_directories);
    run_test("load_restores_index_structure", test_load_restores_index_structure);
    run_test("round_trip_query_results_are_identical", test_round_trip_query_results_are_identical);
    run_test("save_is_byte_deterministic", test_save_is_byte_deterministic);
    run_test("load_replaces_existing_index", test_load_replaces_existing_index);

    run_test("load_missing_directory_fails", test_load_missing_directory_fails);
    run_test("failed_load_leaves_engine_intact", test_failed_load_leaves_engine_intact);
    run_test("load_rejects_bad_magic", test_load_rejects_bad_magic);
    run_test("load_rejects_version_mismatch", test_load_rejects_version_mismatch);
    run_test("load_rejects_truncated_files", test_load_rejects_truncated_files);
    run_test("load_rejects_trailing_garbage", test_load_rejects_trailing_garbage);
    run_test("index_size_of_incomplete_index_is_zero", test_index_size_of_incomplete_index_is_zero);
    run_test("empty_index_round_trips", test_empty_index_round_trips);

    std::cout << "\n" << g_tests_passed << "/" << g_tests_run << " tests passed.\n\n";
    return (g_tests_passed == g_tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
