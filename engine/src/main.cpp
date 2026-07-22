#include "engine.h"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: engine_cli <path-to-jsonl>\n";
        return 1;
    }

    search::Engine engine;

    std::cout << "Building index from " << argv[1] << " ...\n";
    try {
        engine.build_from_jsonl(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    std::cout << "Indexed " << engine.doc_count() << " docs, "
              << engine.term_count() << " unique terms.\n";

    std::cout << "\nEnter queries (Ctrl-D / Ctrl-Z to quit):\n";
    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        if (line.empty()) continue;

        auto results = engine.search(line, 10);
        if (results.empty()) {
            std::cout << "(no results)\n";
        } else {
            for (const auto& r : results) {
                std::cout << "  doc_id=" << r.doc_id
                          << "  score=" << r.score
                          << "  snippet=\"" << r.snippet << "\"\n";
            }
        }
    }
    return 0;
}
