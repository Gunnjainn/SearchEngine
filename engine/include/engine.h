#pragma once

#include <string>
#include <vector>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Result — a single search hit (Contract 2 element).
// ---------------------------------------------------------------------------
struct Result {
    int         doc_id;
    double      score;
    std::string snippet;
};

struct Document {
    int id;
    std::string title;
    std::string url;
    std::string text;
    int length;
};

class Engine {
public:
    void load(const std::string& path);
    void build_from_jsonl(const std::string& path);
    void save(const std::string& path);
    std::vector<Result> search(const std::string& query, int k) const;

private:
    std::vector<std::string> tokenize(const std::string& text) const;

    struct Posting {
        int doc_idx;
        int tf;
    };

    std::vector<Document> docs;
    std::unordered_map<std::string, std::vector<Posting>> inverted_index;
    std::unordered_map<std::string, int> df;
    
    double avgdl = 0.0;
    
    // BM25 parameters
    const double k1 = 1.5;
    const double b = 0.75;
};
