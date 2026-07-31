# Engine

C++17 core search engine with a hand-rolled inverted index, BM25 scoring, and
top-k retrieval. Exposed over HTTP via the [Crow](https://crowcpp.org/)
header-only framework.

## Status

`build_from_jsonl`, the inverted index, the SYNC POINT 2a accessors, BM25
`search`, and `save` / `load` persistence are all implemented.

## Index and doc store (SYNC POINT 2a)

```cpp
struct Result  { int doc_id; double score; std::string snippet; };
struct Posting { int doc_id; int term_freq; };
struct DocMeta { std::string title; std::string url; };

class Engine {
    void build_from_jsonl(const std::string& path);
    bool save(const std::string& index_dir) const;   // false on I/O error
    bool load(const std::string& index_dir);         // false on error/corruption
    std::vector<Result> search(const std::string& query, int k) const;

    // Postings access — consumed by BM25
    const std::vector<Posting>& postings(const std::string& term) const;
    int         doc_length(int doc_id) const;
    int         num_docs() const;
    double      avg_doc_length() const;
    std::size_t num_terms() const;

    // Doc store — consumed for snippets
    std::string doc_text(int doc_id) const;
    DocMeta     doc_meta(int doc_id) const;
};
```

What the index guarantees:

| Guarantee | Detail |
|-----------|--------|
| Postings order | Ascending by `doc_id`, exactly one entry per document. The list length **is** the document frequency. |
| `doc_id` | The external Contract 1 id, never an internal array index. |
| `doc_length` | Token count *after* tokenization — stopwords removed, terms stemmed. |
| Unknown term | `postings()` returns a shared empty list, so the reference is always safe to bind. |
| Unknown `doc_id` | `doc_length` → `0`, `doc_text` → `""`, `doc_meta` → empty. Never throws. |
| Empty index | `avg_doc_length()` is `0.0`, never NaN, and `search` returns no results. |
| Duplicate `doc_id` | First occurrence wins; later lines are skipped with a warning on stderr. |
| Rebuild | `build_from_jsonl` replaces the index rather than appending, so it is idempotent. |
| Result order | `score` DESC, ties broken by `doc_id` ASC, so a given index and query give byte-identical output. |

## On-disk index format

`save(dir)` writes three files; `load(dir)` reconstructs everything
`build_from_jsonl` produced, so a fresh process serves queries with **no JSONL
present**. Booting from a saved index skips tokenizing and stemming the corpus
entirely.

```bash
JSONL_PATH=corpus.jsonl INDEX_PATH=./index  search_engine   # build, save, serve
INDEX_PATH=./index                          search_engine   # load and serve
```

Both paths print the document and term counts, the size on disk, and how long
the build or load took.

### Conventions

Every integer is little-endian, written byte by byte, so an index written on one
machine loads on another. Strings are length-prefixed: a `u32` byte count
followed by raw bytes — never NUL-terminated, so text may contain any byte.
`varint` is unsigned LEB128, one to five bytes, seven payload bits each with the
high bit set on all but the last.

### `meta.bin` — 24 bytes

| Field | Type | Notes |
|-------|------|-------|
| magic | `char[4]` | `SEIX` |
| version | `u32` | currently `1`; a mismatch is rejected, never guessed at |
| num_docs | `u32` | record count in `docs.bin` |
| num_terms | `u32` | record count in `terms.bin` |
| total_tokens | `u64` | sum of all doc lengths, for `avg_doc_length()` |

### `docs.bin` — the doc-length table and doc store

`num_docs` records, in index order:

| Field | Type |
|-------|------|
| doc_id | `i32` |
| length | `i32` (token count after tokenization) |
| title | string |
| url | string |
| text | string |

The document text lives here because `search` builds snippets from
`doc_text()`. Without it a loaded index could rank correctly but not answer
Contract 2.

### `terms.bin` — term dictionary and postings

`num_terms` records, **sorted by term**:

| Field | Type | Notes |
|-------|------|-------|
| term | string | the stemmed index term |
| n_postings | `u32` | also the document frequency |
| postings | see below | `n_postings` entries |

Postings ascend by `doc_id`, so only the first is stored in full and the rest as
**gaps** — which are small, and a varint spends one byte on a small number
instead of four:

```
posting[0] : doc_id            i32       (full; handles a negative first id)
             term_freq         varint
posting[i] : doc_id - prev     varint    (gap, always >= 1)
             term_freq         varint
```

### Two deliberate properties

**Sorted terms make `save` byte-deterministic.** Hash iteration order would
otherwise leak into the file and the same corpus would produce different bytes
each run. A test asserts `save → load → save` reproduces identical files.

**`load` validates before it commits.** It parses into local structures and only
moves them into place once everything checks out, so a corrupt file leaves a
working index untouched rather than half-replaced. It rejects a bad magic, a
version mismatch, truncation, trailing bytes, a `total_tokens` that disagrees
with the sum of doc lengths, a zero doc_id gap, a zero term frequency, and any
posting citing an unknown doc_id.

## Text pipeline

`search::tokenize` (`include/tokenizer.h`) is the single text pipeline. Both
`build_from_jsonl` and `search` call it, so index terms and query terms are
always produced the same way — an inflected query matches an inflected
document. Do not tokenize anywhere else.

```cpp
#include "tokenizer.h"
search::tokenize("Running the Compilers!");   // -> {"run", "compil"}
```

Stages, in order:

| Stage | What it does |
|-------|--------------|
| Fold  | UTF-8 decode, lowercase, fold Latin diacritics to ASCII (`Café` → `cafe`, `Łódź` → `lodz`), drop combining marks |
| Split | Break on anything not alphanumeric |
| Stop  | Drop English stopwords, matched on the folded but **unstemmed** word |
| Stem  | Porter stemming |

Stopwords are the standard NLTK English list minus its apostrophe variants
(we split on apostrophes anyway, so `don't` arrives as `don` + `t`).

`src/porter.cpp` implements Porter's algorithm from the 1980 paper and his
ANSI C reference implementation (both cited in `include/porter.h`). It is
verified to agree with that reference on all 251,970 words of the NLTK
`words` + `brown` vocabularies.

Non-Latin scripts (Greek, Cyrillic, CJK, Hebrew, ...) are lowercased where
cheap and indexed verbatim; they are not stemmed. This is an English-first
tokenizer, not an ICU replacement.

## Build and test

Requires standalone Asio for Crow, as the `Dockerfile` shows:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DASIO_INCLUDE_DIR=/usr/include
cmake --build build --parallel
cd build && ctest --output-on-failure
```