#include "porter.h"

#include <cstddef>
#include <cstring>

namespace search {
namespace {

// ---------------------------------------------------------------------------
// Porter stemmer.
//
// Deliberately mirrors the structure of Porter's reference C implementation so
// it can be checked against it line by line:
//
//   b = working buffer. Characters *past* k are stale leftovers from earlier
//       truncations and are intentionally still readable — step 5 measures
//       b[0..j] where j can exceed k, and the reference implementation relies
//       on the original letters being there.
//   k = index of the last character of the current stem.
//   j = index of the last character of the stem preceding the suffix that
//       ends() most recently matched.
//
// The only deviation from the C original is that setto() grows the buffer
// instead of assuming the truncated tail is large enough to write into.
// ---------------------------------------------------------------------------
class Porter {
public:
    explicit Porter(std::string word)
        : b(std::move(word)), k(static_cast<int>(b.size()) - 1), j(0) {}

    std::string stem() {
        if (k <= 1) return b;  // words of 1-2 letters are left alone
        step1ab();
        step1c();
        step2();
        step3();
        step4();
        step5();
        return b.substr(0, static_cast<std::size_t>(k) + 1);
    }

private:
    std::string b;
    int         k;
    int         j;

    // cons(i) — is b[i] a consonant?  'y' is a consonant only when the
    // preceding letter is a vowel (or it is word-initial).
    bool cons(int i) const {
        switch (b[i]) {
            case 'a': case 'e': case 'i': case 'o': case 'u': return false;
            case 'y': return (i == 0) ? true : !cons(i - 1);
            default:  return true;
        }
    }

    // m() — the "measure" of b[0..j]: the count of VC sequences in
    //   [C](VC){m}[V]
    int m() const {
        int n = 0;
        int i = 0;
        while (true) {
            if (i > j) return n;
            if (!cons(i)) break;
            ++i;
        }
        ++i;
        while (true) {
            while (true) {
                if (i > j) return n;
                if (cons(i)) break;
                ++i;
            }
            ++i;
            ++n;
            while (true) {
                if (i > j) return n;
                if (!cons(i)) break;
                ++i;
            }
            ++i;
        }
    }

    // Does b[0..j] contain a vowel?
    bool vowelinstem() const {
        for (int i = 0; i <= j; ++i) {
            if (!cons(i)) return true;
        }
        return false;
    }

    // Is b[i-1..i] a doubled consonant?
    bool doublec(int i) const {
        if (i < 1) return false;
        if (b[i] != b[i - 1]) return false;
        return cons(i);
    }

    // cvc(i) — b[i-2..i] is consonant-vowel-consonant with the final consonant
    // not w, x or y.  Used to decide whether a short stem keeps a silent 'e'.
    bool cvc(int i) const {
        if (i < 2 || !cons(i) || cons(i - 1) || !cons(i - 2)) return false;
        const char ch = b[i];
        return !(ch == 'w' || ch == 'x' || ch == 'y');
    }

    // Does b[0..k] end with `s`?  On success j is set to the index before it.
    bool ends(const char* s) {
        const int len = static_cast<int>(std::strlen(s));
        if (len > k + 1) return false;
        if (b.compare(static_cast<std::size_t>(k - len + 1),
                      static_cast<std::size_t>(len), s) != 0) {
            return false;
        }
        j = k - len;
        return true;
    }

    // Replace everything after j with `s`.
    void setto(const char* s) {
        const int len = static_cast<int>(std::strlen(s));
        const std::size_t needed = static_cast<std::size_t>(j) + 1 + len;
        if (b.size() < needed) b.resize(needed);
        b.replace(static_cast<std::size_t>(j) + 1, static_cast<std::size_t>(len), s);
        k = j + len;
    }

    // setto(), but only when the stem has a positive measure.
    void r(const char* s) {
        if (m() > 0) setto(s);
    }

    // Step 1a/1b — plurals, and -ed / -ing past forms.
    void step1ab() {
        if (b[k] == 's') {
            if (ends("sses")) {
                k -= 2;
            } else if (ends("ies")) {
                setto("i");
            } else if (k >= 1 && b[k - 1] != 's') {
                --k;
            }
        }
        if (ends("eed")) {
            if (m() > 0) --k;
        } else if ((ends("ed") || ends("ing")) && vowelinstem()) {
            k = j;
            if (ends("at")) {
                setto("ate");
            } else if (ends("bl")) {
                setto("ble");
            } else if (ends("iz")) {
                setto("ize");
            } else if (doublec(k)) {
                // Drop one of the doubled consonants, but keep -ll, -ss, -zz.
                const char ch = b[k];
                if (ch != 'l' && ch != 's' && ch != 'z') --k;
            } else if (m() == 1 && cvc(k)) {
                setto("e");
            }
        }
    }

    // Step 1c — terminal y -> i when the stem contains a vowel.
    void step1c() {
        if (ends("y") && vowelinstem()) b[k] = 'i';
    }

    // Step 2 — collapse double suffices into single ones (m > 0).
    void step2() {
        if (k < 1) return;
        switch (b[k - 1]) {
            case 'a':
                if (ends("ational")) { r("ate");  break; }
                if (ends("tional"))  { r("tion"); break; }
                break;
            case 'c':
                if (ends("enci"))    { r("ence"); break; }
                if (ends("anci"))    { r("ance"); break; }
                break;
            case 'e':
                if (ends("izer"))    { r("ize");  break; }
                break;
            case 'l':
                if (ends("bli"))     { r("ble");  break; }
                if (ends("alli"))    { r("al");   break; }
                if (ends("entli"))   { r("ent");  break; }
                if (ends("eli"))     { r("e");    break; }
                if (ends("ousli"))   { r("ous");  break; }
                break;
            case 'o':
                if (ends("ization")) { r("ize");  break; }
                if (ends("ation"))   { r("ate");  break; }
                if (ends("ator"))    { r("ate");  break; }
                break;
            case 's':
                if (ends("alism"))   { r("al");   break; }
                if (ends("iveness")) { r("ive");  break; }
                if (ends("fulness")) { r("ful");  break; }
                if (ends("ousness")) { r("ous");  break; }
                break;
            case 't':
                if (ends("aliti"))   { r("al");   break; }
                if (ends("iviti"))   { r("ive");  break; }
                if (ends("biliti"))  { r("ble");  break; }
                break;
            case 'g':
                if (ends("logi"))    { r("log");  break; }
                break;
            default:
                break;
        }
    }

    // Step 3 — strip -icate, -ful, -ness etc. (m > 0).
    void step3() {
        switch (b[k]) {
            case 'e':
                if (ends("icate")) { r("ic"); break; }
                if (ends("ative")) { r("");   break; }
                if (ends("alize")) { r("al"); break; }
                break;
            case 'i':
                if (ends("iciti")) { r("ic"); break; }
                break;
            case 'l':
                if (ends("ical"))  { r("ic"); break; }
                if (ends("ful"))   { r("");   break; }
                break;
            case 's':
                if (ends("ness"))  { r("");   break; }
                break;
            default:
                break;
        }
    }

    // Step 4 — strip -ance, -ent, -ion etc. (m > 1).
    void step4() {
        if (k < 1) return;
        switch (b[k - 1]) {
            case 'a':
                if (ends("al")) break;
                return;
            case 'c':
                if (ends("ance")) break;
                if (ends("ence")) break;
                return;
            case 'e':
                if (ends("er")) break;
                return;
            case 'i':
                if (ends("ic")) break;
                return;
            case 'l':
                if (ends("able")) break;
                if (ends("ible")) break;
                return;
            case 'n':
                if (ends("ant"))   break;
                if (ends("ement")) break;
                if (ends("ment"))  break;
                if (ends("ent"))   break;
                return;
            case 'o':
                if (ends("ion") && j >= 0 && (b[j] == 's' || b[j] == 't')) break;
                if (ends("ou")) break;
                return;
            case 's':
                if (ends("ism")) break;
                return;
            case 't':
                if (ends("ate")) break;
                if (ends("iti")) break;
                return;
            case 'u':
                if (ends("ous")) break;
                return;
            case 'v':
                if (ends("ive")) break;
                return;
            case 'z':
                if (ends("ize")) break;
                return;
            default:
                return;
        }
        if (m() > 1) k = j;
    }

    // Step 5 — remove a final -e, then collapse -ll to -l.
    void step5() {
        j = k;
        if (b[k] == 'e') {
            const int a = m();
            if (a > 1 || (a == 1 && !cvc(k - 1))) --k;
        }
        if (k >= 0 && b[k] == 'l' && doublec(k) && m() > 1) --k;
    }
};

// The vowel/consonant rules only make sense for a-z; anything else (digits,
// non-ASCII bytes that survived folding) is passed through untouched.
bool stemmable(const std::string& word) {
    if (word.empty()) return false;
    for (const char c : word) {
        if (c < 'a' || c > 'z') return false;
    }
    return true;
}

}  // namespace

std::string porter_stem(const std::string& word) {
    if (!stemmable(word)) return word;
    return Porter(word).stem();
}

}  // namespace search
