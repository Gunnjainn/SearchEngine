#include "engine.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <nlohmann/json.hpp>

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

void Engine::save(const std::string& path) {
    std::cout << "Saving index to " << path << " is not implemented yet.\n";
}

void Engine::load(const std::string& path) {
    std::cout << "Loading index from " << path << " is not implemented yet.\n";
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
    if (docs.empty()) return {};

    // Same tokenizer as the index build — see search::tokenize in tokenizer.h.
    std::vector<std::string> q_tokens = search::tokenize(query);
    if (q_tokens.empty()) return {};

    // Score documents via the standalone BM25 scorer.
    std::unordered_map<int, double> scores = score_docs(q_tokens);
    if (scores.empty()) return {};

    // Select the top k results using a bounded min-heap — O(n log k) where n
    // is the number of scored documents.  See topk.h for the full complexity
    // analysis.
    return top_k(scores, k, *this);
}
