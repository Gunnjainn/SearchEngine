# Engine

C++17 core search engine with a hand-rolled inverted index, BM25 scoring, and
top-k retrieval. Exposed over HTTP via the [Crow](https://crowcpp.org/)
header-only framework.

## Status

🚧 Not yet implemented.

class Engine {
  void build_from_jsonl(const std::string& path);   // A implements
  void load(const std::string& index_dir);          // A implements
  void save(const std::string& index_dir);          // A implements
  std::vector<Result> search(const std::string& query, int k);  // B implements
};
struct Result { int doc_id; double score; std::string snippet; };


// Postings access (A provides, B consumes for BM25)
struct Posting { int doc_id; int term_freq; };
const std::vector<Posting>& postings(const std::string& term) const;   // A
int    doc_length(int doc_id) const;   // A
int    num_docs() const;               // A
double avg_doc_length() const;         // A

// Doc store (A provides, B consumes for snippets)
std::string doc_text(int doc_id) const;   // A  (for snippet extraction)
DocMeta     doc_meta(int doc_id) const;    // A  (title, url)
```

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