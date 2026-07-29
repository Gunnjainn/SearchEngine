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
