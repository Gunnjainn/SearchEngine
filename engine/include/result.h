#pragma once
#include <string>

// ---------------------------------------------------------------------------
// Result — a single search hit (Contract 2 element).
// ---------------------------------------------------------------------------
struct Result {
    int         doc_id;
    double      score;
    std::string snippet;
};
