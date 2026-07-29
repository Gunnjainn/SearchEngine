# Engine

C++17 core search engine with a hand-rolled inverted index, BM25 scoring, and
top-k retrieval. Exposed over HTTP via the [Crow](https://crowcpp.org/)
header-only framework.

## Status

`build_from_jsonl`, the inverted index, the SYNC POINT 2a accessors and BM25
`search` are implemented. `save` / `load` are still stubs.

## Index and doc store (SYNC POINT 2a)

```cpp
struct Result  { int doc_id; double score; std::string snippet; };
struct Posting { int doc_id; int term_freq; };
struct DocMeta { std::string title; std::string url; };

class Engine {
    void build_from_jsonl(const std::string& path);
    void load(const std::string& index_dir);   // stub
    void save(const std::string& index_dir);   // stub
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