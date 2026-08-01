#pragma once
#include <cstddef>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

// Title and URL of an indexed document (SYNC POINT 2a).
struct DocMeta {
    std::string title;
    std::string url;
};

namespace search {

// Where one document lives in the backing file, plus its indexing statistic.
// This is all that stays in RAM per document: 16 bytes of bookkeeping, no text.
struct DocEntry {
    int       doc_id;
    long long offset;        // byte offset into the backing file
    int       token_length;  // token count after tokenization
};

// ---------------------------------------------------------------------------
// DocStore — document text and metadata addressed by doc_id.
//
// Only the offset table is held in memory; title, url and text are read from
// the backing file on demand. RSS therefore grows with the *number* of
// documents, not with the size of the corpus text.
//
// Two backings, both identified by a path:
//
//   Jsonl  — the Contract 1 corpus itself, offsets being line starts. Used
//            straight after build_from_jsonl, so a build costs no extra I/O and
//            never holds the text.
//   Packed — a docs.bin written by write_packed(), holding just the three
//            fields per document. Used after load().
//
// Because both backings are addressed by a stable path, every read opens its
// own stream. That makes the store safe to call from Crow's worker threads
// without a lock — deliberate, since a shared stream plus mutex would serialise
// snippet extraction across all in-flight queries, and snippets are the one
// part of a query that touches the disk.
// ---------------------------------------------------------------------------
class DocStore {
public:
    // ── Choosing a backing (each clears the table) ─────────────────────────
    void reset_jsonl(std::string jsonl_path);
    void reset_packed(std::string docs_bin_path);
    void clear();

    // Append an entry. The caller guarantees doc_id is not already present.
    void add(const DocEntry& entry);

    // Repoint at a packed file, keeping the given offsets, which must be in
    // all() order. Used by save() so the engine stops depending on the JSONL.
    void rebind_packed(std::string docs_bin_path, const std::vector<long long>& offsets);

    // ── Queries ───────────────────────────────────────────────────────────
    std::size_t size() const { return docs.size(); }
    bool        empty() const { return docs.empty(); }
    bool        contains(int doc_id) const;

    // 0 if doc_id is unknown.
    int token_length(int doc_id) const;

    // Empty if doc_id is unknown or the backing file cannot be read.
    std::string text(int doc_id) const;
    DocMeta     meta(int doc_id) const;

    // Entries in insertion order.
    const std::vector<DocEntry>& all() const { return docs; }

    bool               is_packed() const;
    const std::string& backing_path() const { return path; }

    // Roughly how much RAM the offset table occupies. Reported by the CLI so
    // the bounded-memory property is observable rather than merely claimed.
    std::size_t approx_ram_bytes() const;

    // ── Persistence ───────────────────────────────────────────────────────
    // Copy every document into `out_path` as packed records, appending each
    // document's new offset to `offsets` in all() order.
    bool write_packed(const std::string& out_path, std::vector<long long>& offsets) const;

private:
    enum class Backing { None, Jsonl, Packed };

    // Reads whichever fields are requested from an already-open stream. Any
    // out-param may be null to skip that field.
    bool read_record(std::istream& in, long long offset, std::string* title,
                     std::string* url, std::string* text) const;

    Backing     backing = Backing::None;
    std::string path;

    std::vector<DocEntry>                docs;       // insertion order
    std::unordered_map<int, std::size_t> by_doc_id;  // doc_id -> index into docs
};

// ---------------------------------------------------------------------------
// make_snippet — the Contract 2 `snippet` field.
//
// Takes at most max_bytes of `text`, never splitting a UTF-8 character, and
// appends an ellipsis when it truncates. The boundary check is not cosmetic: a
// half character would make the JSON response invalid UTF-8 and nlohmann's
// dump() throws on that, turning a long document into a 500.
// ---------------------------------------------------------------------------
std::string make_snippet(const std::string& text, std::size_t max_bytes);

// ---------------------------------------------------------------------------
// make_focused_snippet — a window of `text` around the query terms.
//
// The head of a document usually says nothing about why it matched, so this
// finds the densest run of query terms instead and returns roughly max_bytes
// of text around it, marking each truncated side with an ellipsis.
//
// `query_terms` are stemmed terms as produced by search::tokenize; the document
// is re-tokenized with tokenize_spans() so the two are compared in the same
// space. Falls back to make_snippet() when no term is found — which happens
// when a document matched on its title, since only the body is passed here.
//
// Like make_snippet, never splits a UTF-8 character, and additionally avoids
// cutting mid-word when a nearby space allows it.
// ---------------------------------------------------------------------------
std::string make_focused_snippet(const std::string& text,
                                 const std::vector<std::string>& query_terms,
                                 std::size_t max_bytes);

}  // namespace search
