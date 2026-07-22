#include "engine.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <set>

namespace search {

// ── Tokenizer ────────────────────────────────────────────────────────────────

std::vector<std::string> Engine::tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::istringstream stream(text);
    std::string word;
    while (stream >> word) {
        // Lowercase in-place.
        std::transform(word.begin(), word.end(), word.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        tokens.push_back(std::move(word));
    }
    return tokens;
}

// ── Postings intersection ────────────────────────────────────────────────────

std::vector<int> Engine::intersect(const std::vector<int>& a,
                                   const std::vector<int>& b) {
    std::vector<int> out;
    auto ia = a.begin(), ib = b.begin();
    while (ia != a.end() && ib != b.end()) {
        if (*ia == *ib) {
            out.push_back(*ia);
            ++ia;
            ++ib;
        } else if (*ia < *ib) {
            ++ia;
        } else {
            ++ib;
        }
    }
    return out;
}

// ── Index building ───────────────────────────────────────────────────────────

void Engine::build_from_jsonl(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open JSONL file: " + path);
    }

    index_.clear();
    docs_.clear();

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        nlohmann::json doc = nlohmann::json::parse(line);

        int doc_id      = doc.at("doc_id").get<int>();
        std::string title = doc.at("title").get<std::string>();
        std::string url   = doc.at("url").get<std::string>();
        std::string text  = doc.at("text").get<std::string>();

        // Store document metadata.
        docs_[doc_id] = DocMeta{title, url};

        // Tokenize title + text together.
        std::string full_text = title + " " + text;
        auto tokens = tokenize(full_text);

        // Deduplicate terms for this document (we want each doc_id to appear
        // at most once per term's postings list).
        std::set<std::string> unique_terms(tokens.begin(), tokens.end());

        for (const auto& term : unique_terms) {
            index_[term].push_back(doc_id);
        }
    }

    // Postings are already sorted per-term because we process doc_ids in file
    // order and insert only once per doc.  If the JSONL is not sorted by
    // doc_id we need to sort each list.
    for (auto& [term, postings] : index_) {
        std::sort(postings.begin(), postings.end());
    }
}

// ── Search ───────────────────────────────────────────────────────────────────

std::vector<Result> Engine::search(const std::string& query, int k) const {
    auto terms = tokenize(query);

    if (terms.empty() || k <= 0) {
        return {};
    }

    // Start with the postings of the first term.
    auto it = index_.find(terms[0]);
    if (it == index_.end()) {
        return {};
    }
    std::vector<int> candidates = it->second;

    // AND with every subsequent term's postings.
    for (size_t i = 1; i < terms.size(); ++i) {
        it = index_.find(terms[i]);
        if (it == index_.end()) {
            return {};  // term not in index → AND is empty
        }
        candidates = intersect(candidates, it->second);
        if (candidates.empty()) {
            return {};
        }
    }

    // Build results (score=0.0, snippet=title for now).
    std::vector<Result> results;
    int limit = std::min(static_cast<int>(candidates.size()), k);
    for (int i = 0; i < limit; ++i) {
        int did = candidates[i];
        auto doc_it = docs_.find(did);
        std::string snippet = (doc_it != docs_.end()) ? doc_it->second.title : "";
        results.push_back(Result{did, 0.0, snippet});
    }
    return results;
}

// ── Persistence stubs ────────────────────────────────────────────────────────

void Engine::save(const std::string& /*index_dir*/) const {
    // TODO: implement persistence
}

void Engine::load(const std::string& /*index_dir*/) {
    // TODO: implement persistence
}

// ── Accessor ─────────────────────────────────────────────────────────────────

std::vector<int> Engine::postings(const std::string& term) const {
    auto it = index_.find(term);
    if (it == index_.end()) return {};
    return it->second;
}

}  // namespace search
