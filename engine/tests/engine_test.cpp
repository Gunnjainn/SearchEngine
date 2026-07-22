//
// engine_test.cpp — unit tests for the Engine class.
//
// Uses a 5-doc JSONL fixture written to a temporary file.
// Built against a vendored copy of doctest (single-header).
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "engine.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

// ── Helpers ──────────────────────────────────────────────────────────────────

/// Write a 5-doc JSONL fixture to a temp file and return its path.
static std::string write_fixture() {
    // Use a deterministic temp-file name inside the build tree.
    std::string path = "test_fixture.jsonl";
    std::ofstream out(path);
    REQUIRE(out.is_open());

    out << R"({"doc_id":1,"title":"Alpha Article","url":"http://a.com","text":"the quick brown fox"})" << "\n";
    out << R"({"doc_id":2,"title":"Beta Article","url":"http://b.com","text":"the slow brown dog"})" << "\n";
    out << R"({"doc_id":3,"title":"Gamma Article","url":"http://c.com","text":"quick quick fox jumps"})" << "\n";
    out << R"({"doc_id":4,"title":"Delta Article","url":"http://d.com","text":"brown brown brown"})" << "\n";
    out << R"({"doc_id":5,"title":"Epsilon Article","url":"http://e.com","text":"the fox and the dog"})" << "\n";

    out.close();
    return path;
}

static search::Engine build_engine() {
    search::Engine e;
    e.build_from_jsonl(write_fixture());
    return e;
}

// ── Tests ────────────────────────────────────────────────────────────────────

TEST_CASE("build_from_jsonl indexes all documents") {
    auto engine = build_engine();
    CHECK(engine.doc_count() == 5);
    CHECK(engine.term_count() > 0);
}

TEST_CASE("single-term search returns correct postings") {
    auto engine = build_engine();

    SUBCASE("term 'fox' appears in docs 1, 3, 5") {
        auto results = engine.search("fox", 10);
        REQUIRE(results.size() == 3);
        CHECK(results[0].doc_id == 1);
        CHECK(results[1].doc_id == 3);
        CHECK(results[2].doc_id == 5);
    }

    SUBCASE("term 'brown' appears in docs 1, 2, 4") {
        auto results = engine.search("brown", 10);
        REQUIRE(results.size() == 3);
        CHECK(results[0].doc_id == 1);
        CHECK(results[1].doc_id == 2);
        CHECK(results[2].doc_id == 4);
    }
}

TEST_CASE("multi-term AND search intersects correctly") {
    auto engine = build_engine();

    SUBCASE("'brown fox' matches docs with both terms: 1") {
        auto results = engine.search("brown fox", 10);
        REQUIRE(results.size() == 1);
        CHECK(results[0].doc_id == 1);
    }

    SUBCASE("'the dog' matches docs 2 and 5") {
        auto results = engine.search("the dog", 10);
        REQUIRE(results.size() == 2);
        CHECK(results[0].doc_id == 2);
        CHECK(results[1].doc_id == 5);
    }
}

TEST_CASE("search for absent term returns empty") {
    auto engine = build_engine();
    auto results = engine.search("zzzzz", 10);
    CHECK(results.empty());
}

TEST_CASE("AND with one missing term returns empty") {
    auto engine = build_engine();
    // 'fox' exists but 'zzzzz' does not
    auto results = engine.search("fox zzzzz", 10);
    CHECK(results.empty());
}

TEST_CASE("k limits the number of returned results") {
    auto engine = build_engine();
    // 'the' appears in docs 1, 2, 5 (from text) — at least 3 hits.
    auto all = engine.search("the", 100);
    REQUIRE(all.size() >= 3);

    auto limited = engine.search("the", 2);
    CHECK(limited.size() == 2);
}

TEST_CASE("search is case-insensitive") {
    auto engine = build_engine();

    auto lower = engine.search("fox", 10);
    auto upper = engine.search("FOX", 10);
    auto mixed = engine.search("FoX", 10);

    REQUIRE(lower.size() == upper.size());
    REQUIRE(lower.size() == mixed.size());

    for (size_t i = 0; i < lower.size(); ++i) {
        CHECK(lower[i].doc_id == upper[i].doc_id);
        CHECK(lower[i].doc_id == mixed[i].doc_id);
    }
}

TEST_CASE("result snippet is the document title") {
    auto engine = build_engine();
    auto results = engine.search("quick", 10);
    REQUIRE(!results.empty());

    // doc 1 has title "Alpha Article"
    bool found_alpha = false;
    for (const auto& r : results) {
        if (r.doc_id == 1) {
            CHECK(r.snippet == "Alpha Article");
            found_alpha = true;
        }
    }
    CHECK(found_alpha);
}

TEST_CASE("result score is 0.0 (placeholder)") {
    auto engine = build_engine();
    auto results = engine.search("fox", 10);
    for (const auto& r : results) {
        CHECK(r.score == 0.0);
    }
}

TEST_CASE("empty query returns no results") {
    auto engine = build_engine();
    CHECK(engine.search("", 10).empty());
    CHECK(engine.search("   ", 10).empty());
}

TEST_CASE("postings are sorted and unique") {
    auto engine = build_engine();
    // 'brown' in docs 1, 2, 4 — doc 4 has it 3 times but should appear once
    auto p = engine.postings("brown");
    REQUIRE(p.size() == 3);
    CHECK(p[0] == 1);
    CHECK(p[1] == 2);
    CHECK(p[2] == 4);
}
