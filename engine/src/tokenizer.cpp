#include "tokenizer.h"

#include <cstdint>
#include <unordered_set>

#include "porter.h"

namespace search {
namespace {

// ===========================================================================
// Stopwords
//
// The standard English stopword list shipped with NLTK
// (nltk_data/corpora/stopwords/english, itself descended from the Porter /
// SMART tradition), with the apostrophe-bearing variants removed: we split on
// non-alphanumerics, so "don't" already arrives as "don" + "t" and both pieces
// are in the list below.
// ===========================================================================
const char* const kStopwords[] = {
    "i", "me", "my", "myself", "we", "our", "ours", "ourselves",
    "you", "your", "yours", "yourself", "yourselves",
    "he", "him", "his", "himself", "she", "her", "hers", "herself",
    "it", "its", "itself", "they", "them", "their", "theirs", "themselves",
    "what", "which", "who", "whom", "this", "that", "these", "those",
    "am", "is", "are", "was", "were", "be", "been", "being",
    "have", "has", "had", "having", "do", "does", "did", "doing",
    "a", "an", "the", "and", "but", "if", "or", "because", "as", "until",
    "while", "of", "at", "by", "for", "with", "about", "against", "between",
    "into", "through", "during", "before", "after", "above", "below", "to",
    "from", "up", "down", "in", "out", "on", "off", "over", "under",
    "again", "further", "then", "once", "here", "there", "when", "where",
    "why", "how", "all", "any", "both", "each", "few", "more", "most",
    "other", "some", "such", "no", "nor", "not", "only", "own", "same",
    "so", "than", "too", "very", "s", "t", "can", "will", "just", "don",
    "should", "now", "d", "ll", "m", "o", "re", "ve", "y", "ain",
    "aren", "couldn", "didn", "doesn", "hadn", "hasn", "haven", "isn",
    "ma", "mightn", "mustn", "needn", "shan", "shouldn", "wasn", "weren",
    "won", "wouldn",
};

const std::unordered_set<std::string>& stopword_set() {
    static const std::unordered_set<std::string> set(
        std::begin(kStopwords), std::end(kStopwords));
    return set;
}

// ===========================================================================
// UTF-8 + character classification
// ===========================================================================

constexpr std::uint32_t kReplacementChar = 0xFFFD;

// Decode the code point starting at `i` and advance `i` past it. Malformed
// sequences consume one byte and yield U+FFFD (classified as a separator).
std::uint32_t next_codepoint(const std::string& s, std::size_t& i) {
    const auto byte = [&s](std::size_t at) {
        return static_cast<std::uint8_t>(s[at]);
    };

    const std::uint8_t b0 = byte(i);
    int    extra = 0;
    std::uint32_t cp = 0;

    if (b0 < 0x80) {
        ++i;
        return b0;
    } else if ((b0 & 0xE0) == 0xC0) {
        extra = 1;
        cp = b0 & 0x1Fu;
    } else if ((b0 & 0xF0) == 0xE0) {
        extra = 2;
        cp = b0 & 0x0Fu;
    } else if ((b0 & 0xF8) == 0xF0) {
        extra = 3;
        cp = b0 & 0x07u;
    } else {
        ++i;  // stray continuation byte or invalid lead
        return kReplacementChar;
    }

    // We need bytes i+1 .. i+extra to be present.
    if (i + static_cast<std::size_t>(extra) >= s.size()) {
        ++i;
        return kReplacementChar;
    }

    for (int n = 1; n <= extra; ++n) {
        const std::uint8_t bn = byte(i + static_cast<std::size_t>(n));
        if ((bn & 0xC0) != 0x80) {  // truncated sequence
            ++i;
            return kReplacementChar;
        }
        cp = (cp << 6) | (bn & 0x3Fu);
    }

    i += static_cast<std::size_t>(extra) + 1;
    return cp;
}

void append_utf8(std::uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// ASCII base letters for U+00C0..U+00FF. Empty string = not a letter.
const char* const kLatin1Fold[] = {
    /* C0 À */ "a",  /* C1 Á */ "a",  /* C2 Â */ "a",  /* C3 Ã */ "a",
    /* C4 Ä */ "a",  /* C5 Å */ "a",  /* C6 Æ */ "ae", /* C7 Ç */ "c",
    /* C8 È */ "e",  /* C9 É */ "e",  /* CA Ê */ "e",  /* CB Ë */ "e",
    /* CC Ì */ "i",  /* CD Í */ "i",  /* CE Î */ "i",  /* CF Ï */ "i",
    /* D0 Ð */ "d",  /* D1 Ñ */ "n",  /* D2 Ò */ "o",  /* D3 Ó */ "o",
    /* D4 Ô */ "o",  /* D5 Õ */ "o",  /* D6 Ö */ "o",  /* D7 × */ "",
    /* D8 Ø */ "o",  /* D9 Ù */ "u",  /* DA Ú */ "u",  /* DB Û */ "u",
    /* DC Ü */ "u",  /* DD Ý */ "y",  /* DE Þ */ "th", /* DF ß */ "ss",
    /* E0 à */ "a",  /* E1 á */ "a",  /* E2 â */ "a",  /* E3 ã */ "a",
    /* E4 ä */ "a",  /* E5 å */ "a",  /* E6 æ */ "ae", /* E7 ç */ "c",
    /* E8 è */ "e",  /* E9 é */ "e",  /* EA ê */ "e",  /* EB ë */ "e",
    /* EC ì */ "i",  /* ED í */ "i",  /* EE î */ "i",  /* EF ï */ "i",
    /* F0 ð */ "d",  /* F1 ñ */ "n",  /* F2 ò */ "o",  /* F3 ó */ "o",
    /* F4 ô */ "o",  /* F5 õ */ "o",  /* F6 ö */ "o",  /* F7 ÷ */ "",
    /* F8 ø */ "o",  /* F9 ù */ "u",  /* FA ú */ "u",  /* FB û */ "u",
    /* FC ü */ "u",  /* FD ý */ "y",  /* FE þ */ "th", /* FF ÿ */ "y",
};
static_assert(sizeof(kLatin1Fold) / sizeof(kLatin1Fold[0]) == 64,
              "kLatin1Fold must cover exactly U+00C0..U+00FF");

// ASCII base letters for Latin Extended-A, U+0100..U+017F.
const char* const kLatinExtAFold[] = {
    /* 0100 Āā */ "a",  "a",  /* 0102 Ăă */ "a",  "a",
    /* 0104 Ąą */ "a",  "a",  /* 0106 Ćć */ "c",  "c",
    /* 0108 Ĉĉ */ "c",  "c",  /* 010A Ċċ */ "c",  "c",
    /* 010C Čč */ "c",  "c",  /* 010E Ďď */ "d",  "d",
    /* 0110 Đđ */ "d",  "d",  /* 0112 Ēē */ "e",  "e",
    /* 0114 Ĕĕ */ "e",  "e",  /* 0116 Ėė */ "e",  "e",
    /* 0118 Ęę */ "e",  "e",  /* 011A Ěě */ "e",  "e",
    /* 011C Ĝĝ */ "g",  "g",  /* 011E Ğğ */ "g",  "g",
    /* 0120 Ġġ */ "g",  "g",  /* 0122 Ģģ */ "g",  "g",
    /* 0124 Ĥĥ */ "h",  "h",  /* 0126 Ħħ */ "h",  "h",
    /* 0128 Ĩĩ */ "i",  "i",  /* 012A Īī */ "i",  "i",
    /* 012C Ĭĭ */ "i",  "i",  /* 012E Įį */ "i",  "i",
    /* 0130 İı */ "i",  "i",  /* 0132 Ĳĳ */ "ij", "ij",
    /* 0134 Ĵĵ */ "j",  "j",  /* 0136 Ķķ */ "k",  "k",
    /* 0138 ĸ  */ "k",         /* 0139 Ĺ  */ "l",
    /* 013A ĺ  */ "l",         /* 013B Ļ  */ "l",
    /* 013C ļ  */ "l",         /* 013D Ľ  */ "l",
    /* 013E ľ  */ "l",         /* 013F Ŀ  */ "l",
    /* 0140 ŀ  */ "l",         /* 0141 Ł  */ "l",
    /* 0142 ł  */ "l",         /* 0143 Ń  */ "n",
    /* 0144 ń  */ "n",         /* 0145 Ņ  */ "n",
    /* 0146 ņ  */ "n",         /* 0147 Ň  */ "n",
    /* 0148 ň  */ "n",         /* 0149 ŉ  */ "n",
    /* 014A Ŋŋ */ "ng", "ng", /* 014C Ōō */ "o",  "o",
    /* 014E Ŏŏ */ "o",  "o",  /* 0150 Őő */ "o",  "o",
    /* 0152 Œœ */ "oe", "oe", /* 0154 Ŕŕ */ "r",  "r",
    /* 0156 Ŗŗ */ "r",  "r",  /* 0158 Řř */ "r",  "r",
    /* 015A Śś */ "s",  "s",  /* 015C Ŝŝ */ "s",  "s",
    /* 015E Şş */ "s",  "s",  /* 0160 Šš */ "s",  "s",
    /* 0162 Ţţ */ "t",  "t",  /* 0164 Ťť */ "t",  "t",
    /* 0166 Ŧŧ */ "t",  "t",  /* 0168 Ũũ */ "u",  "u",
    /* 016A Ūū */ "u",  "u",  /* 016C Ŭŭ */ "u",  "u",
    /* 016E Ůů */ "u",  "u",  /* 0170 Űű */ "u",  "u",
    /* 0172 Ųų */ "u",  "u",  /* 0174 Ŵŵ */ "w",  "w",
    /* 0176 Ŷŷ */ "y",  "y",  /* 0178 Ÿ  */ "y",
    /* 0179 Ź  */ "z",         /* 017A ź  */ "z",
    /* 017B Ż  */ "z",         /* 017C ż  */ "z",
    /* 017D Ž  */ "z",         /* 017E ž  */ "z",
    /* 017F ſ  */ "s",
};
static_assert(sizeof(kLatinExtAFold) / sizeof(kLatinExtAFold[0]) == 128,
              "kLatinExtAFold must cover exactly U+0100..U+017F");

enum class CharClass {
    Alnum,      // part of a token; folded form appended to `out`
    Separator,  // ends the current token
    Ignorable,  // contributes nothing but does not end the token
};

bool in(std::uint32_t cp, std::uint32_t lo, std::uint32_t hi) {
    return cp >= lo && cp <= hi;
}

// Marks and format characters that should vanish without splitting a token:
// combining diacritics (so NFD text folds like NFC), soft hyphen, zero-width
// joiners and variation selectors.
bool is_ignorable(std::uint32_t cp) {
    return cp == 0x00AD                 // soft hyphen
        || in(cp, 0x0300, 0x036F)       // combining diacritical marks
        || in(cp, 0x1AB0, 0x1AFF)       // combining diacriticals extended
        || in(cp, 0x1DC0, 0x1DFF)       // combining diacriticals supplement
        || in(cp, 0x200B, 0x200D)       // zero width space/non-joiner/joiner
        || in(cp, 0x20D0, 0x20FF)       // combining marks for symbols
        || in(cp, 0xFE00, 0xFE0F)       // variation selectors
        || in(cp, 0xFE20, 0xFE2F);      // combining half marks
}

bool is_separator_symbol(std::uint32_t cp) {
    // U+00A1..U+00BF is punctuation, signs and fractions, except for the two
    // ordinal indicators ª and º which fold to letters.
    if (in(cp, 0x00A1, 0x00BF)) return cp != 0x00AA && cp != 0x00BA;

    return in(cp, 0x0080, 0x00A0)       // C1 controls + nbsp
        || in(cp, 0x02B0, 0x02FF)       // spacing modifier letters
        || in(cp, 0x2000, 0x206F)       // general punctuation
        || in(cp, 0x2070, 0x209F)       // super/subscripts
        || in(cp, 0x20A0, 0x20CF)       // currency symbols
        || in(cp, 0x2100, 0x2BFF)       // letterlike, arrows, math, symbols
        || in(cp, 0x2E00, 0x2E7F)       // supplemental punctuation
        || in(cp, 0x3000, 0x303F)       // CJK symbols and punctuation
        || in(cp, 0xD800, 0xDFFF)       // surrogates (only from bad input)
        || in(cp, 0xFE10, 0xFE6F)       // vertical / small form variants
        || in(cp, 0xFFF0, 0xFFFF)       // specials, incl. U+FFFD
        || cp >= 0x1F000;               // emoji, pictographs, out-of-range
}

// Lowercase Greek (U+0370..U+03FF). Final sigma is left alone.
std::uint32_t lower_greek(std::uint32_t cp) {
    if (cp == 0x0386) return 0x03AC;
    if (in(cp, 0x0388, 0x038A)) return cp + 0x25;
    if (cp == 0x038C) return 0x03CC;
    if (in(cp, 0x038E, 0x038F)) return cp + 0x3F;
    if (in(cp, 0x0391, 0x03A1) || in(cp, 0x03A3, 0x03AB)) return cp + 0x20;
    return cp;
}

// Lowercase Cyrillic (U+0400..U+04FF).
std::uint32_t lower_cyrillic(std::uint32_t cp) {
    if (in(cp, 0x0400, 0x040F)) return cp + 0x50;
    if (in(cp, 0x0410, 0x042F)) return cp + 0x20;
    if (in(cp, 0x0460, 0x0481) || in(cp, 0x048A, 0x04BF)) {
        return (cp % 2 == 0) ? cp + 1 : cp;  // upper/lower pairs
    }
    return cp;
}

CharClass classify(std::uint32_t cp, std::string& out) {
    // --- ASCII -----------------------------------------------------------
    if (cp < 0x80) {
        if (cp >= '0' && cp <= '9') {
            out += static_cast<char>(cp);
            return CharClass::Alnum;
        }
        if (cp >= 'A' && cp <= 'Z') {
            out += static_cast<char>(cp - 'A' + 'a');
            return CharClass::Alnum;
        }
        if (cp >= 'a' && cp <= 'z') {
            out += static_cast<char>(cp);
            return CharClass::Alnum;
        }
        return CharClass::Separator;
    }

    if (is_ignorable(cp)) return CharClass::Ignorable;
    if (is_separator_symbol(cp)) return CharClass::Separator;

    // --- Latin, folded to ASCII ------------------------------------------
    if (cp == 0x00AA) { out += 'a'; return CharClass::Alnum; }  // ª
    if (cp == 0x00BA) { out += 'o'; return CharClass::Alnum; }  // º
    if (in(cp, 0x00C0, 0x00FF)) {
        const char* fold = kLatin1Fold[cp - 0x00C0];
        if (*fold == '\0') return CharClass::Separator;  // × and ÷
        out += fold;
        return CharClass::Alnum;
    }
    if (in(cp, 0x0100, 0x017F)) {
        out += kLatinExtAFold[cp - 0x0100];
        return CharClass::Alnum;
    }

    // --- Fullwidth ASCII --------------------------------------------------
    if (in(cp, 0xFF10, 0xFF19)) {
        out += static_cast<char>('0' + (cp - 0xFF10));
        return CharClass::Alnum;
    }
    if (in(cp, 0xFF21, 0xFF3A)) {
        out += static_cast<char>('a' + (cp - 0xFF21));
        return CharClass::Alnum;
    }
    if (in(cp, 0xFF41, 0xFF5A)) {
        out += static_cast<char>('a' + (cp - 0xFF41));
        return CharClass::Alnum;
    }
    if (in(cp, 0xFF01, 0xFF20) || in(cp, 0xFF3B, 0xFF40)
        || in(cp, 0xFF5B, 0xFF65)) {
        return CharClass::Separator;
    }

    // --- Cased non-Latin scripts we can lowercase cheaply -----------------
    if (in(cp, 0x0370, 0x03FF)) {
        append_utf8(lower_greek(cp), out);
        return CharClass::Alnum;
    }
    if (in(cp, 0x0400, 0x04FF)) {
        append_utf8(lower_cyrillic(cp), out);
        return CharClass::Alnum;
    }

    // --- Everything else: keep verbatim as part of the token -------------
    // Latin Extended-B, Hebrew, Arabic, Devanagari, CJK, ... These index and
    // match exactly; porter_stem() passes non-ASCII tokens through untouched.
    append_utf8(cp, out);
    return CharClass::Alnum;
}

}  // namespace

// ===========================================================================
// Public API
// ===========================================================================

std::vector<std::string> fold_and_split(const std::string& text) {
    std::vector<std::string> words;
    std::string current;

    std::size_t i = 0;
    while (i < text.size()) {
        const std::uint32_t cp = next_codepoint(text, i);
        switch (classify(cp, current)) {
            case CharClass::Alnum:
                break;  // classify() already appended the folded form
            case CharClass::Ignorable:
                break;
            case CharClass::Separator:
                if (!current.empty()) {
                    words.push_back(current);
                    current.clear();
                }
                break;
        }
    }
    if (!current.empty()) words.push_back(current);

    return words;
}

bool is_stopword(const std::string& folded_word) {
    return stopword_set().count(folded_word) != 0;
}

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    for (const std::string& word : fold_and_split(text)) {
        if (is_stopword(word)) continue;
        std::string stem = porter_stem(word);
        if (stem.empty()) continue;
        tokens.push_back(std::move(stem));
    }
    return tokens;
}

}  // namespace search
