#include "engine.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <utility>
#include <nlohmann/json.hpp>

#include "index_io.h"
#include "tokenizer.h"

using json = nlohmann::json;

void Engine::build_from_jsonl(const std::string& path) {
    std::ifstream infile(path);
    if (!infile.is_open()) {
        std::cerr << "Failed to open JSONL: " << path << std::endl;
        return;
    }

    // Rebuilding replaces the index rather than appending to it, so calling
    // this twice is idempotent.
    docs.clear();
    doc_index.clear();
    inverted_index.clear();
    total_tokens = 0;

    std::string line;

    std::cout << "Building index from " << path << "..." << std::endl;

    while (std::getline(infile, line)) {
        if (line.empty()) continue;

        try {
            auto j = json::parse(line);
            Document doc;
            doc.id = j["doc_id"].get<int>();
            doc.title = j.value("title", "");
            doc.url = j.value("url", "");
            doc.text = j.value("text", "");

            // doc_id keys the postings lists and the doc store, so it has to
            // be unique. First occurrence wins.
            if (doc_index.count(doc.id) != 0) {
                std::cerr << "Duplicate doc_id " << doc.id
                          << " — keeping the first, skipping this line\n";
                continue;
            }

            std::string content = doc.title + " " + doc.text;
            std::vector<std::string> tokens = search::tokenize(content);
            doc.length = static_cast<int>(tokens.size());
            total_tokens += doc.length;

            doc_index[doc.id] = docs.size();
            docs.push_back(doc);

            std::unordered_map<std::string, int> term_freqs;
            for (const auto& token : tokens) {
                term_freqs[token]++;
            }

            for (const auto& [term, tf] : term_freqs) {
                inverted_index[term].push_back({doc.id, tf});
            }

            if (docs.size() % 100 == 0) {
                std::cout << "\rIndexed " << docs.size() << " documents..." << std::flush;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing line: " << e.what() << "\nLine: " << line << std::endl;
        }
    }

    // SYNC POINT 2a guarantees postings ascend by doc_id. Documents may arrive
    // in any order, so sort once here rather than inserting in order per doc.
    for (auto& [term, plist] : inverted_index) {
        std::sort(plist.begin(), plist.end(),
                  [](const Posting& a, const Posting& b) { return a.doc_id < b.doc_id; });
    }

    std::cout << "\rIndexed " << docs.size() << " documents, "
              << inverted_index.size() << " terms. Done.\n";
}

// ---------------------------------------------------------------------------
// Persistence — see engine/README.md for the format.
// ---------------------------------------------------------------------------

bool Engine::save(const std::string& dir) const {
    namespace fs = std::filesystem;
    namespace io = search::io;

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        std::cerr << "[index] cannot create " << dir << ": " << ec.message() << "\n";
        return false;
    }

    const fs::path base(dir);

    // meta.bin — identity and the counts needed to read the other two files.
    {
        std::ofstream out(base / io::kMetaFile, std::ios::binary);
        if (!out) {
            std::cerr << "[index] cannot write " << io::kMetaFile << "\n";
            return false;
        }
        out.write(io::kMagic, 4);
        io::write_u32(out, io::kVersion);
        io::write_u32(out, static_cast<std::uint32_t>(docs.size()));
        io::write_u32(out, static_cast<std::uint32_t>(inverted_index.size()));
        io::write_u64(out, static_cast<std::uint64_t>(total_tokens));
        if (!out.good()) {
            std::cerr << "[index] write failed for " << io::kMetaFile << "\n";
            return false;
        }
    }

    // docs.bin — the doc-length table and the doc store, in index order.
    // Snippets come from doc_text(), so the text has to be here for a loaded
    // index to answer queries identically.
    {
        std::ofstream out(base / io::kDocsFile, std::ios::binary);
        if (!out) {
            std::cerr << "[index] cannot write " << io::kDocsFile << "\n";
            return false;
        }
        for (const Document& doc : docs) {
            io::write_i32(out, doc.id);
            io::write_i32(out, doc.length);
            io::write_string(out, doc.title);
            io::write_string(out, doc.url);
            io::write_string(out, doc.text);
        }
        if (!out.good()) {
            std::cerr << "[index] write failed for " << io::kDocsFile << "\n";
            return false;
        }
    }

    // terms.bin — term dictionary and postings.
    //
    // Terms are written in sorted order, not hash order, so that the same
    // corpus always produces byte-identical files. That makes save -> load ->
    // save verifiable by comparison, and keeps the output diffable.
    {
        std::vector<const std::string*> terms;
        terms.reserve(inverted_index.size());
        for (const auto& [term, plist] : inverted_index) {
            (void)plist;
            terms.push_back(&term);
        }
        std::sort(terms.begin(), terms.end(),
                  [](const std::string* a, const std::string* b) { return *a < *b; });

        std::ofstream out(base / io::kTermsFile, std::ios::binary);
        if (!out) {
            std::cerr << "[index] cannot write " << io::kTermsFile << "\n";
            return false;
        }

        for (const std::string* term : terms) {
            const std::vector<Posting>& plist = inverted_index.at(*term);
            io::write_string(out, *term);
            io::write_u32(out, static_cast<std::uint32_t>(plist.size()));

            // Postings ascend by doc_id, so store the first id in full and the
            // rest as gaps. Gaps are small, and a varint spends one byte on a
            // small number instead of four.
            std::int32_t prev = 0;
            for (std::size_t i = 0; i < plist.size(); ++i) {
                if (i == 0) {
                    io::write_i32(out, plist[i].doc_id);
                } else {
                    io::write_varint(out,
                        static_cast<std::uint32_t>(plist[i].doc_id - prev));
                }
                io::write_varint(out, static_cast<std::uint32_t>(plist[i].term_freq));
                prev = plist[i].doc_id;
            }
        }
        if (!out.good()) {
            std::cerr << "[index] write failed for " << io::kTermsFile << "\n";
            return false;
        }
    }

    return true;
}

bool Engine::load(const std::string& dir) {
    namespace fs = std::filesystem;
    namespace io = search::io;

    const fs::path base(dir);

    // ── meta.bin ──
    std::uint32_t version = 0, n_docs = 0, n_terms = 0;
    std::uint64_t tokens = 0;
    {
        std::ifstream in(base / io::kMetaFile, std::ios::binary);
        if (!in) {
            std::cerr << "[index] cannot open " << (base / io::kMetaFile).string() << "\n";
            return false;
        }
        char magic[4] = {};
        in.read(magic, 4);
        if (in.gcount() != 4 || std::memcmp(magic, io::kMagic, 4) != 0) {
            std::cerr << "[index] " << dir << " is not a search index\n";
            return false;
        }
        if (!io::read_u32(in, version)) return false;
        if (version != io::kVersion) {
            std::cerr << "[index] version " << version << " but this build reads "
                      << io::kVersion << "\n";
            return false;
        }
        if (!io::read_u32(in, n_docs) || !io::read_u32(in, n_terms)
            || !io::read_u64(in, tokens)) {
            std::cerr << "[index] " << io::kMetaFile << " is truncated\n";
            return false;
        }
    }

    // Everything is read into locals and only committed once fully valid, so a
    // failed load leaves a working index in place.
    std::vector<Document>                new_docs;
    std::unordered_map<int, std::size_t> new_doc_index;
    std::unordered_map<std::string, std::vector<Posting>> new_index;

    // ── docs.bin ──
    {
        std::ifstream in(base / io::kDocsFile, std::ios::binary);
        if (!in) {
            std::cerr << "[index] cannot open " << io::kDocsFile << "\n";
            return false;
        }

        new_docs.reserve(n_docs);
        long long token_sum = 0;

        for (std::uint32_t i = 0; i < n_docs; ++i) {
            std::int32_t id = 0, length = 0;
            Document doc;
            if (!io::read_i32(in, id) || !io::read_i32(in, length)
                || !io::read_string(in, doc.title)
                || !io::read_string(in, doc.url)
                || !io::read_string(in, doc.text)) {
                std::cerr << "[index] " << io::kDocsFile << " is truncated at document "
                          << i << "\n";
                return false;
            }
            if (length < 0) {
                std::cerr << "[index] negative doc_length for doc_id " << id << "\n";
                return false;
            }
            if (new_doc_index.count(id) != 0) {
                std::cerr << "[index] duplicate doc_id " << id << " in " << io::kDocsFile << "\n";
                return false;
            }

            doc.id = id;
            doc.length = length;
            token_sum += length;
            new_doc_index[id] = new_docs.size();
            new_docs.push_back(std::move(doc));
        }

        if (!io::at_eof(in)) {
            std::cerr << "[index] trailing data in " << io::kDocsFile << "\n";
            return false;
        }
        // Cross-check the header against the records it describes.
        if (token_sum != static_cast<long long>(tokens)) {
            std::cerr << "[index] token total " << tokens << " disagrees with the sum of "
                      << "doc lengths (" << token_sum << ")\n";
            return false;
        }
    }

    // ── terms.bin ──
    {
        std::ifstream in(base / io::kTermsFile, std::ios::binary);
        if (!in) {
            std::cerr << "[index] cannot open " << io::kTermsFile << "\n";
            return false;
        }

        new_index.reserve(n_terms);

        for (std::uint32_t t = 0; t < n_terms; ++t) {
            std::string term;
            std::uint32_t count = 0;
            if (!io::read_string(in, term) || !io::read_u32(in, count)) {
                std::cerr << "[index] " << io::kTermsFile << " is truncated at term " << t << "\n";
                return false;
            }
            if (count == 0) {
                std::cerr << "[index] term \"" << term << "\" has an empty postings list\n";
                return false;
            }

            std::vector<Posting> plist;
            plist.reserve(count);
            std::int32_t prev = 0;

            for (std::uint32_t i = 0; i < count; ++i) {
                std::int32_t doc_id = 0;
                if (i == 0) {
                    if (!io::read_i32(in, doc_id)) {
                        std::cerr << "[index] truncated postings for \"" << term << "\"\n";
                        return false;
                    }
                } else {
                    std::uint32_t gap = 0;
                    if (!io::read_varint(in, gap)) {
                        std::cerr << "[index] truncated postings for \"" << term << "\"\n";
                        return false;
                    }
                    // Gaps come from a strictly ascending list, so 0 would mean
                    // a repeated doc_id.
                    if (gap == 0) {
                        std::cerr << "[index] zero doc_id gap in \"" << term << "\"\n";
                        return false;
                    }
                    doc_id = prev + static_cast<std::int32_t>(gap);
                }

                std::uint32_t tf = 0;
                if (!io::read_varint(in, tf) || tf == 0) {
                    std::cerr << "[index] bad term_freq for \"" << term << "\"\n";
                    return false;
                }
                if (new_doc_index.count(doc_id) == 0) {
                    std::cerr << "[index] \"" << term << "\" cites unknown doc_id " << doc_id << "\n";
                    return false;
                }

                plist.push_back({doc_id, static_cast<int>(tf)});
                prev = doc_id;
            }

            if (!new_index.emplace(std::move(term), std::move(plist)).second) {
                std::cerr << "[index] duplicate term in " << io::kTermsFile << "\n";
                return false;
            }
        }

        if (!io::at_eof(in)) {
            std::cerr << "[index] trailing data in " << io::kTermsFile << "\n";
            return false;
        }
    }

    // ── Commit ──
    docs           = std::move(new_docs);
    doc_index      = std::move(new_doc_index);
    inverted_index = std::move(new_index);
    total_tokens   = static_cast<long long>(tokens);

    return true;
}

// ---------------------------------------------------------------------------
// Postings access + doc store (SYNC POINT 2a)
// ---------------------------------------------------------------------------

const std::vector<Posting>& Engine::postings(const std::string& term) const {
    static const std::vector<Posting> empty;
    auto it = inverted_index.find(term);
    return (it == inverted_index.end()) ? empty : it->second;
}

const Document* Engine::find_doc(int doc_id) const {
    auto it = doc_index.find(doc_id);
    return (it == doc_index.end()) ? nullptr : &docs[it->second];
}

int Engine::doc_length(int doc_id) const {
    const Document* doc = find_doc(doc_id);
    return (doc != nullptr) ? doc->length : 0;
}

int Engine::num_docs() const {
    return static_cast<int>(docs.size());
}

double Engine::avg_doc_length() const {
    if (docs.empty()) return 0.0;
    return static_cast<double>(total_tokens) / static_cast<double>(docs.size());
}

std::size_t Engine::num_terms() const {
    return inverted_index.size();
}

std::string Engine::doc_text(int doc_id) const {
    const Document* doc = find_doc(doc_id);
    return (doc != nullptr) ? doc->text : std::string();
}

DocMeta Engine::doc_meta(int doc_id) const {
    const Document* doc = find_doc(doc_id);
    if (doc == nullptr) return DocMeta{};
    return DocMeta{doc->title, doc->url};
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

std::vector<Result> Engine::search(const std::string& query, int k) const {
    if (docs.empty()) return {};

    // Same tokenizer as the index build — see search::tokenize in tokenizer.h.
    std::vector<std::string> q_tokens = search::tokenize(query);
    if (q_tokens.empty()) return {};

    // Every document tokenized to nothing (e.g. a corpus of pure stopwords):
    // avgdl would be 0 and the BM25 length norm would divide by zero.
    const double avgdl = avg_doc_length();
    if (avgdl <= 0.0) return {};

    const double N = static_cast<double>(num_docs());

    std::unordered_map<int, double> scores;  // doc_id -> accumulated BM25

    for (const auto& q_term : q_tokens) {
        const std::vector<Posting>& plist = postings(q_term);
        if (plist.empty()) continue;

        // A term appears once per document in its postings list, so the list
        // length *is* the document frequency.
        double doc_freq = static_cast<double>(plist.size());
        double idf = std::log( (N - doc_freq + 0.5) / (doc_freq + 0.5) + 1.0 );

        for (const auto& posting : plist) {
            double tf = static_cast<double>(posting.term_freq);
            double doc_len = static_cast<double>(doc_length(posting.doc_id));

            double score_term = idf * (tf * (k1 + 1.0)) / (tf + k1 * (1.0 - b + b * (doc_len / avgdl)));
            scores[posting.doc_id] += score_term;
        }
    }

    std::vector<Result> results;
    results.reserve(scores.size());
    for (const auto& [doc_id, score] : scores) {
        Result r;
        r.doc_id = doc_id;
        r.score = score;

        const std::string text = doc_text(doc_id);
        r.snippet = text.substr(0, 150);
        if (text.length() > 150) r.snippet += "...";

        results.push_back(std::move(r));
    }

    // Contract 2 orders by score DESC. Ties break on doc_id ASC so that a
    // given index and query always produce byte-identical output — `scores` is
    // an unordered_map and std::sort is not stable, so without the tie-break
    // equal-scoring documents could permute between runs.
    std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.doc_id < b.doc_id;
    });

    if (k > 0 && static_cast<size_t>(k) < results.size()) {
        results.resize(k);
    }

    return results;
}
