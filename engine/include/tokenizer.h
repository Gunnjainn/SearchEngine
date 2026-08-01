#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace search {

// ---------------------------------------------------------------------------
// tokenize — THE text pipeline. Used by both the index build and the query
// path so that indexed terms and query terms are always in the same space.
//
// Pipeline, in order:
//   1. UTF-8 decode + "unicode-aware-ish" lowercasing (see fold_and_split).
//   2. Split on anything that is not alphanumeric.
//   3. Drop English stopwords (matched on the folded, *unstemmed* word).
//   4. Porter-stem what is left.
//
//   tokenize("Running the Compilers!") -> {"run", "compil"}
// ---------------------------------------------------------------------------
std::vector<std::string> tokenize(const std::string& text);

// ---------------------------------------------------------------------------
// TokenSpan — a token plus where it came from in the source text.
//
// `begin` and `end` are byte offsets into the *original* string, not the folded
// form, so they stay valid after diacritic folding changes a token's length.
// ---------------------------------------------------------------------------
struct TokenSpan {
    std::string term;   // the same value tokenize() would produce
    std::size_t begin;  // byte offset of the first character
    std::size_t end;    // one past the last character
};

// tokenize(), but keeping each token's position. Used to build snippets around
// the query terms: the index stores stems, so finding a term in a document
// means re-running the pipeline and remembering where each stem started.
//
// Guaranteed to agree with tokenize(): the terms of tokenize_spans(t), in
// order, are exactly tokenize(t).
std::vector<TokenSpan> tokenize_spans(const std::string& text);

// ---------------------------------------------------------------------------
// Stages 1+2 on their own: lowercase, fold diacritics to ASCII where we can,
// and split on non-alphanumerics. No stopword removal, no stemming.
//
// "Unicode-aware-ish" means:
//   * ASCII is lowercased.
//   * Latin-1 Supplement and Latin Extended-A letters are lowercased and
//     folded to their ASCII base ("Café" -> "cafe", "Łódź" -> "lodz"), with
//     ligatures expanded ("Æ" -> "ae", "ß" -> "ss").
//   * Combining marks, soft hyphens and variation selectors are dropped
//     without breaking the token, so decomposed forms fold like precomposed
//     ones.
//   * Greek and Cyrillic are lowercased and kept as UTF-8.
//   * Fullwidth ASCII letters/digits are folded to ASCII.
//   * Punctuation, symbols, whitespace and emoji are separators.
//   * Any other script (Hebrew, Arabic, CJK, ...) is kept as-is: those tokens
//     survive indexing but are not stemmed. This is an English-first
//     tokenizer, not a full ICU replacement.
// ---------------------------------------------------------------------------
std::vector<std::string> fold_and_split(const std::string& text);

// True if `folded_word` is in the embedded English stopword list. Expects the
// output of fold_and_split (lowercase, no punctuation).
bool is_stopword(const std::string& folded_word);

}  // namespace search
