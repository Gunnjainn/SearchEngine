#include "doc_store.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "index_io.h"
#include "tokenizer.h"

namespace search {
namespace {

namespace io = search::io;
using json = nlohmann::json;

// Strip the \r of a CRLF line ending. The JSONL is read in binary mode — text
// mode would translate line endings and make byte offsets meaningless — so the
// carriage return arrives here and has to go.
void strip_cr(std::string& line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
}

}  // namespace

// ---------------------------------------------------------------------------
// Table management
// ---------------------------------------------------------------------------

void DocStore::clear() {
    backing = Backing::None;
    path.clear();
    docs.clear();
    by_doc_id.clear();
}

void DocStore::reset_jsonl(std::string jsonl_path) {
    clear();
    backing = Backing::Jsonl;
    path = std::move(jsonl_path);
}

void DocStore::reset_packed(std::string docs_bin_path) {
    clear();
    backing = Backing::Packed;
    path = std::move(docs_bin_path);
}

void DocStore::add(const DocEntry& entry) {
    by_doc_id[entry.doc_id] = docs.size();
    docs.push_back(entry);
}

void DocStore::rebind_packed(std::string docs_bin_path,
                             const std::vector<long long>& offsets) {
    if (offsets.size() != docs.size()) {
        std::cerr << "[docstore] rebind ignored: " << offsets.size()
                  << " offsets for " << docs.size() << " documents\n";
        return;
    }
    backing = Backing::Packed;
    path = std::move(docs_bin_path);
    for (std::size_t i = 0; i < docs.size(); ++i) {
        docs[i].offset = offsets[i];
    }
}

bool DocStore::is_packed() const {
    return backing == Backing::Packed;
}

bool DocStore::contains(int doc_id) const {
    return by_doc_id.count(doc_id) != 0;
}

int DocStore::token_length(int doc_id) const {
    auto it = by_doc_id.find(doc_id);
    return (it == by_doc_id.end()) ? 0 : docs[it->second].token_length;
}

std::size_t DocStore::approx_ram_bytes() const {
    // The vector, plus the hash map's node and bucket overhead. The map cost is
    // an estimate, not a measurement — enough to show the shape of the growth.
    const std::size_t vector_bytes = docs.capacity() * sizeof(DocEntry);
    const std::size_t map_bytes =
        by_doc_id.size() * (sizeof(std::pair<const int, std::size_t>) + 2 * sizeof(void*))
        + by_doc_id.bucket_count() * sizeof(void*);
    return vector_bytes + map_bytes + path.capacity() + sizeof(*this);
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

bool DocStore::read_record(std::istream& in, long long offset, std::string* title,
                           std::string* url, std::string* text) const {
    in.seekg(static_cast<std::streamoff>(offset));
    if (!in) return false;

    if (backing == Backing::Packed) {
        // title, url and text are consecutive length-prefixed strings, so a
        // field is reached by reading past the ones before it.
        std::string scratch;
        std::string* const targets[3] = {title, url, text};
        for (std::string* target : targets) {
            std::string& dst = (target != nullptr) ? *target : scratch;
            if (!io::read_string(in, dst)) return false;
        }
        return true;
    }

    if (backing == Backing::Jsonl) {
        std::string line;
        if (!std::getline(in, line)) return false;
        strip_cr(line);
        try {
            const auto j = json::parse(line);
            if (title != nullptr) *title = j.value("title", "");
            if (url != nullptr)   *url   = j.value("url", "");
            if (text != nullptr)  *text  = j.value("text", "");
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[docstore] cannot parse the line at offset " << offset
                      << ": " << e.what() << "\n";
            return false;
        }
    }

    return false;
}

std::string DocStore::text(int doc_id) const {
    auto it = by_doc_id.find(doc_id);
    if (it == by_doc_id.end() || backing == Backing::None) return std::string();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "[docstore] cannot open " << path << "\n";
        return std::string();
    }

    std::string body;
    if (!read_record(in, docs[it->second].offset, nullptr, nullptr, &body)) {
        std::cerr << "[docstore] cannot read text for doc_id " << doc_id << "\n";
        return std::string();
    }
    return body;
}

DocMeta DocStore::meta(int doc_id) const {
    auto it = by_doc_id.find(doc_id);
    if (it == by_doc_id.end() || backing == Backing::None) return DocMeta{};

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "[docstore] cannot open " << path << "\n";
        return DocMeta{};
    }

    DocMeta out;
    if (!read_record(in, docs[it->second].offset, &out.title, &out.url, nullptr)) {
        std::cerr << "[docstore] cannot read metadata for doc_id " << doc_id << "\n";
        return DocMeta{};
    }
    return out;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

bool DocStore::write_packed(const std::string& out_path,
                            std::vector<long long>& offsets) const {
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "[docstore] cannot write " << out_path << "\n";
        return false;
    }

    offsets.clear();
    offsets.reserve(docs.size());

    if (docs.empty()) return out.good();

    // One stream for the whole pass rather than one per document: this is a
    // sequential bulk copy, not a random-access query.
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "[docstore] cannot open " << path << " to copy documents\n";
        return false;
    }

    for (const DocEntry& entry : docs) {
        std::string title, url, text;
        if (!read_record(in, entry.offset, &title, &url, &text)) {
            std::cerr << "[docstore] cannot read doc_id " << entry.doc_id
                      << " while saving\n";
            return false;
        }

        offsets.push_back(static_cast<long long>(out.tellp()));
        io::write_string(out, title);
        io::write_string(out, url);
        io::write_string(out, text);

        if (!out.good()) {
            std::cerr << "[docstore] write failed for doc_id " << entry.doc_id << "\n";
            return false;
        }
    }

    return out.good();
}

// ---------------------------------------------------------------------------
// Snippets
// ---------------------------------------------------------------------------

std::string make_snippet(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return text;

    // text[cut] is the first byte left out. If it is a UTF-8 continuation byte
    // (10xxxxxx) the cut lands inside a character, so walk back to its start.
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut) + "...";
}

namespace {

bool is_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Move forward to the next UTF-8 character start at or after `pos`.
std::size_t utf8_forward(const std::string& s, std::size_t pos) {
    while (pos < s.size() && is_continuation(s[pos])) ++pos;
    return pos;
}

// Move back to the character start at or before `pos`.
std::size_t utf8_back(const std::string& s, std::size_t pos) {
    if (pos > s.size()) pos = s.size();
    while (pos > 0 && is_continuation(s[pos])) --pos;
    return pos;
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

}  // namespace

std::string make_focused_snippet(const std::string& text,
                                 const std::vector<std::string>& query_terms,
                                 std::size_t max_bytes) {
    if (text.size() <= max_bytes) return text;          // whole document fits
    if (query_terms.empty()) return make_snippet(text, max_bytes);

    const std::unordered_set<std::string> wanted(query_terms.begin(), query_terms.end());

    const std::vector<TokenSpan> spans = tokenize_spans(text);
    std::vector<std::size_t> hits;  // indices into spans that match the query
    for (std::size_t i = 0; i < spans.size(); ++i) {
        if (wanted.count(spans[i].term) != 0) hits.push_back(i);
    }
    if (hits.empty()) return make_snippet(text, max_bytes);

    // Lead-in so the first match is not flush against the left edge.
    const std::size_t lead = max_bytes / 4;

    // Try a window anchored on each hit and keep the one covering the most
    // distinct query terms, breaking ties on total matches then on position.
    std::size_t best_start = 0;
    long long   best_score = -1;

    for (std::size_t h : hits) {
        const std::size_t anchor = spans[h].begin;
        const std::size_t start  = (anchor > lead) ? anchor - lead : 0;
        const std::size_t stop   = std::min(text.size(), start + max_bytes);

        std::unordered_set<std::string> distinct;
        long long total = 0;
        for (std::size_t j : hits) {
            if (spans[j].begin >= start && spans[j].end <= stop) {
                distinct.insert(spans[j].term);
                ++total;
            }
        }

        // Distinct terms dominate: a window showing two query words is more
        // informative than one showing the same word twice.
        const long long score = static_cast<long long>(distinct.size()) * 1000 + total;
        if (score > best_score) {
            best_score = score;
            best_start = start;
        }
    }

    std::size_t start = utf8_forward(text, best_start);
    std::size_t stop  = utf8_back(text, std::min(text.size(), start + max_bytes));

    // Avoid starting or ending mid-word, but only if a space is close enough
    // that trimming to it does not gut the window. A single token longer than
    // the whole window (a hash, a base64 blob) has no space to snap to, so the
    // plain byte cut stands.
    const std::size_t slack = max_bytes / 4;
    if (start > 0) {
        std::size_t s = start;
        while (s < stop && s < start + slack && !is_space(text[s])) ++s;
        if (s < stop && is_space(text[s])) start = utf8_forward(text, s + 1);
    }
    if (stop < text.size()) {
        std::size_t e = stop;
        while (e > start && e + slack > stop && !is_space(text[e])) --e;
        if (e > start && is_space(text[e])) stop = utf8_back(text, e);
    }
    if (stop <= start) return make_snippet(text, max_bytes);

    std::string out;
    if (start > 0) out += "...";
    out.append(text, start, stop - start);
    if (stop < text.size()) out += "...";
    return out;
}

}  // namespace search
