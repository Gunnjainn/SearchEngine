#pragma once
#include <string>

namespace search {

// ---------------------------------------------------------------------------
// porter_stem — Porter's suffix-stripping algorithm.
//
// Expects a *lowercase ASCII* word ([a-z0-9]). Words containing anything else
// (digits, non-ASCII bytes) are returned unchanged, since the algorithm's
// consonant/vowel rules only make sense for English letters.
//
// Implemented from:
//   M.F. Porter, "An algorithm for suffix stripping", Program 14(3):130-137,
//   1980.  https://tartarus.org/martin/PorterStemmer/def.txt
// The step ordering, the `m()` measure and the step-2/3 suffix tables follow
// Porter's own ANSI C reference implementation (porter.c, public domain):
//   https://tartarus.org/martin/PorterStemmer/c.txt
// which differs from the 1980 paper in two documented places: step 2 uses
// "bli" -> "ble" (rather than "abli" -> "able") and adds "logi" -> "log".
// ---------------------------------------------------------------------------
std::string porter_stem(const std::string& word);

}  // namespace search
