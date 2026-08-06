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

namespace {

// Longest snippet returned in a Contract 2 result, in bytes.
constexpr std::size_t kSnippetBytes = 150;

}  // namespace

void Engine::build_from_jsonl(const std::string& path) {
    // Binary, not text: the doc store addresses documents by byte offset within
    // this file, and text mode would translate line endings and shift them.
    std::ifstream infile(path, std::ios::binary);
    if (!infile.is_open()) {
        std::cerr << "Failed to open JSONL: " << path << std::endl;
        return;
    }

    // Rebuilding replaces the index rather than appending to it, so calling
    // this twice is idempotent.
    inverted_index.clear();
    total_tokens = 0;
    docs.reset_jsonl(path);

    // Cached results were computed against the old index and are now wrong.
    query_cache.clear();

    std::string line;
    long long offset = 0;

    std::cout << "Building index from " << path << "..." << std::endl;

    while (std::getline(infile, line)) {
        const long long line_start = offset;
        // getline consumed the delimiter too, so account for it here.
        offset += static_cast<long long>(line.size()) + 1;

        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        try {
            auto j = json::parse(line);
            const int doc_id = j["doc_id"].get<int>();

            // doc_id keys the postings lists and the doc store, so it has to
            // be unique. First occurrence wins.
            if (docs.contains(doc_id)) {
                std::cerr << "Duplicate doc_id " << doc_id
                          << " — keeping the first, skipping this line\n";
                continue;
            }

            const std::string title = j.value("title", "");
            const std::string text  = j.value("text", "");

            const std::vector<std::string> tokens = search::tokenize(title + " " + text);
            const int length = static_cast<int>(tokens.size());
            total_tokens += length;

            docs.add(search::DocEntry{doc_id, line_start, length});

            std::unordered_map<std::string, int> term_freqs;
            for (const auto& token : tokens) {
                term_freqs[token]++;
            }

            for (const auto& [term, tf] : term_freqs) {
                inverted_index[term].push_back({doc_id, tf});
            }

            // Every 10k, not every 100: the \r redraw only works on a terminal,
            // and in `docker logs` each update becomes its own entry — at 100k
            // documents that turned the boot log into one unreadable line.
            if (docs.size() % 10000 == 0) {
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

bool Engine::save(const std::string& dir) {
    namespace fs = std::filesystem;
    namespace io = search::io;

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        std::cerr << "[index] cannot create " << dir << ": " << ec.message() << "\n";
        return false;
    }

    const fs::path base(dir);

    // docs.bin — the field payloads. Written first because it produces the
    // offsets that docs.idx has to record.
    std::vector<long long> offsets;
    if (!docs.write_packed((base / io::kDocsFile).string(), offsets)) {
        return false;
    }

    // meta.bin — identity and the counts needed to read the other files.
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

    // docs.idx — the doc_id -> offset table, plus each document's token count.
    // This is the only part of the doc store that is read into memory.
    {
        std::ofstream out(base / io::kDocsIdxFile, std::ios::binary);
        if (!out) {
            std::cerr << "[index] cannot write " << io::kDocsIdxFile << "\n";
            return false;
        }
        const std::vector<search::DocEntry>& entries = docs.all();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            io::write_i32(out, entries[i].doc_id);
            io::write_i32(out, entries[i].token_length);
            io::write_u64(out, static_cast<std::uint64_t>(offsets[i]));
        }
        if (!out.good()) {
            std::cerr << "[index] write failed for " << io::kDocsIdxFile << "\n";
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

    // The saved copy is now self-contained, so stop reading through the JSONL.
    docs.rebind_packed((base / io::kDocsFile).string(), offsets);

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
    search::DocStore new_docs;
    std::unordered_map<std::string, std::vector<Posting>> new_index;

    const fs::path docs_bin = base / io::kDocsFile;
    new_docs.reset_packed(docs_bin.string());

    // Payload size, so an offset pointing outside the file is caught here
    // instead of producing an empty snippet at query time.
    std::error_code ec;
    const std::uintmax_t docs_bytes = fs::file_size(docs_bin, ec);
    if (ec) {
        std::cerr << "[index] cannot stat " << docs_bin.string() << ": " << ec.message() << "\n";
        return false;
    }

    // ── docs.idx ──
    {
        std::ifstream in(base / io::kDocsIdxFile, std::ios::binary);
        if (!in) {
            std::cerr << "[index] cannot open " << io::kDocsIdxFile << "\n";
            return false;
        }

        long long token_sum = 0;

        for (std::uint32_t i = 0; i < n_docs; ++i) {
            std::int32_t doc_id = 0, length = 0;
            std::uint64_t offset = 0;
            if (!io::read_i32(in, doc_id) || !io::read_i32(in, length)
                || !io::read_u64(in, offset)) {
                std::cerr << "[index] " << io::kDocsIdxFile << " is truncated at entry "
                          << i << "\n";
                return false;
            }
            if (length < 0) {
                std::cerr << "[index] negative doc_length for doc_id " << doc_id << "\n";
                return false;
            }
            if (offset >= docs_bytes && !(offset == 0 && docs_bytes == 0)) {
                std::cerr << "[index] doc_id " << doc_id << " points past the end of "
                          << io::kDocsFile << "\n";
                return false;
            }
            if (new_docs.contains(doc_id)) {
                std::cerr << "[index] duplicate doc_id " << doc_id << " in "
                          << io::kDocsIdxFile << "\n";
                return false;
            }

            new_docs.add(search::DocEntry{doc_id, static_cast<long long>(offset), length});
            token_sum += length;
        }

        if (!io::at_eof(in)) {
            std::cerr << "[index] trailing data in " << io::kDocsIdxFile << "\n";
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
                if (!new_docs.contains(doc_id)) {
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
    inverted_index = std::move(new_index);
    total_tokens   = static_cast<long long>(tokens);

    // Only now, after the commit: a failed load leaves both the index and the
    // cache untouched, so they cannot disagree.
    query_cache.clear();

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

int Engine::doc_length(int doc_id) const {
    return docs.token_length(doc_id);
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
    return docs.text(doc_id);
}

DocMeta Engine::doc_meta(int doc_id) const {
    return docs.meta(doc_id);
}

// ---------------------------------------------------------------------------
// BM25 scoring
// ---------------------------------------------------------------------------

std::unordered_map<int, double> Engine::score_docs(
    const std::vector<std::string>& query_terms) const
{
    // Delegate to the free function, passing ourselves as the PostingsSource.
    return ::score_docs(query_terms, *this, bm25_params);
}

// ---------------------------------------------------------------------------
// Search — uses bounded min-heap for O(n log k) top-k selection.
// ---------------------------------------------------------------------------

std::vector<Result> Engine::search(const std::string& query, int k) const {
    return search(query, k, search::QueryMode::And);
}

std::vector<Result> Engine::search(const std::string& query, int k,
                                   search::QueryMode mode) const {
    if (docs.empty()) return {};

    // 1. Parse — the same search::tokenize the index build uses, so a query
    //    term and the indexed term for the same word are identical.
    const search::Query parsed = search::parse_query(query, mode);
    if (parsed.terms.empty()) return {};

    // Serve a repeat from the cache. The key is the parsed terms, so queries
    // that differ only in casing, punctuation, stopwords or word order share an
    // entry. See search::cache_key.
    const std::string key = search::cache_key(mode, parsed.terms, k);
    std::vector<Result> cached;
    if (query_cache.get(key, cached)) return cached;

    std::vector<Result> results = execute(parsed, k);

    // Empty results are cached too: proving that nothing matches still costs a
    // postings intersection, and a query with no hits is exactly the kind a user
    // retries.
    query_cache.put(key, results);
    return results;
}

std::vector<Result> Engine::execute(const search::Query& parsed, int k) const {
    // 2. Candidates — AND intersects the postings lists of the distinct terms,
    //    OR unions them. Either way this decides *which* documents are worth
    //    scoring, not their order.
    const std::vector<int> candidates = search::candidate_docs(parsed, *this);
    if (candidates.empty()) return {};

    // 3. Rank — BM25 over the candidates only (bm25.cpp). Document frequency
    //    still comes from the whole collection, so narrowing the candidates
    //    does not distort what a term is worth.
    const std::unordered_map<int, double> scores =
        ::score_docs(parsed.terms, *this, bm25_params, candidates);
    if (scores.empty()) return {};

    // 4. Top k — bounded min-heap, O(n log k). See topk.h.
    std::vector<Result> results = top_k(scores, k);

    // 5. Snippets — only now, for the k survivors. Each is a disk read, and a
    //    window around the matched terms rather than the head of the document,
    //    which usually says nothing about why the document matched.
    for (Result& r : results) {
        r.snippet = search::make_focused_snippet(docs.text(r.doc_id), parsed.terms,
                                                 kSnippetBytes);
    }

    return results;
}
