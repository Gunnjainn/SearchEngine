#include "engine.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

std::vector<std::string> Engine::tokenize(const std::string& text) const {
    std::vector<std::string> tokens;
    std::string current_token;
    
    for (char c : text) {
        if (std::isalnum(c)) {
            current_token += std::tolower(c);
        } else if (!current_token.empty()) {
            tokens.push_back(current_token);
            current_token.clear();
        }
    }
    if (!current_token.empty()) {
        tokens.push_back(current_token);
    }
    return tokens;
}

void Engine::build_from_jsonl(const std::string& path) {
    std::ifstream infile(path);
    if (!infile.is_open()) {
        std::cerr << "Failed to open JSONL: " << path << std::endl;
        return;
    }

    std::string line;
    long long total_length = 0;
    
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
            
            // Combine title and text for tokenization
            std::string content = doc.title + " " + doc.text;
            std::vector<std::string> tokens = tokenize(content);
            doc.length = tokens.size();
            total_length += doc.length;
            
            int doc_idx = docs.size();
            docs.push_back(doc);
            
            // Local term frequency
            std::unordered_map<std::string, int> term_freqs;
            for (const auto& token : tokens) {
                term_freqs[token]++;
            }
            
            // Add to inverted index
            for (const auto& [term, tf] : term_freqs) {
                inverted_index[term].push_back({doc_idx, tf});
                df[term]++;
            }
            
            if (docs.size() % 100 == 0) {
                std::cout << "\rIndexed " << docs.size() << " documents..." << std::flush;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing line: " << e.what() << "\nLine: " << line << std::endl;
        }
    }
    std::cout << "\rIndexed " << docs.size() << " documents. Done.\n";
    
    if (!docs.empty()) {
        avgdl = static_cast<double>(total_length) / docs.size();
    }
}

void Engine::save(const std::string& path) {
    std::cout << "Saving index to " << path << " is not implemented yet.\n";
}

void Engine::load(const std::string& path) {
    std::cout << "Loading index from " << path << " is not implemented yet.\n";
}

std::vector<Result> Engine::search(const std::string& query, int k) const {
    if (docs.empty()) return {};

    std::vector<std::string> q_tokens = tokenize(query);
    if (q_tokens.empty()) return {};
    
    std::unordered_map<int, double> scores;
    double N = static_cast<double>(docs.size());
    
    for (const auto& q_term : q_tokens) {
        auto it = inverted_index.find(q_term);
        if (it == inverted_index.end()) continue;
        
        double doc_freq = static_cast<double>(df.at(q_term));
        double idf = std::log( (N - doc_freq + 0.5) / (doc_freq + 0.5) + 1.0 );
        
        for (const auto& posting : it->second) {
            int doc_idx = posting.doc_idx;
            double tf = static_cast<double>(posting.tf);
            double doc_len = static_cast<double>(docs[doc_idx].length);
            
            double score_term = idf * (tf * (k1 + 1.0)) / (tf + k1 * (1.0 - b + b * (doc_len / avgdl)));
            scores[doc_idx] += score_term;
        }
    }
    
    std::vector<Result> results;
    for (const auto& [doc_idx, score] : scores) {
        Result r;
        r.doc_id = docs[doc_idx].id;
        r.score = score;
        
        // Generate a small snippet
        const auto& text = docs[doc_idx].text;
        r.snippet = text.substr(0, 150);
        if (text.length() > 150) r.snippet += "...";
        
        results.push_back(r);
    }
    
    // Sort descending by score
    std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) {
        return a.score > b.score;
    });
    
    // Truncate to top-k
    if (k > 0 && static_cast<size_t>(k) < results.size()) {
        results.resize(k);
    }
    
    return results;
}
