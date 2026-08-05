// ---------------------------------------------------------------------------
// Search Engine — Crow HTTP Server
//
// Serves POST /search (Contract 2) and GET /health.
// Delegates search logic to the Engine class.
// ---------------------------------------------------------------------------

#include "engine.h"
#include "index_io.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "crow.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

// Documents are addressed by offset, so RAM tracks the document *count* and not
// the size of the corpus text. Print both so the difference is visible.
static void report_doc_store(const Engine& engine) {
    const search::DocStore& store = engine.doc_store();
    std::error_code ec;
    const std::uintmax_t backing = std::filesystem::file_size(store.backing_path(), ec);

    std::cout << "[engine] Doc store: " << store.size() << " docs, "
              << store.approx_ram_bytes() << " bytes of offsets in RAM";
    if (!ec) {
        std::cout << ", " << backing << " bytes of text on disk ("
                  << store.backing_path() << ")";
    }
    std::cout << "\n";
}

int main() {
    // ── Initialise engine ─────────────────────────────────────────────────
    Engine engine;

    const char* jsonl_path = std::getenv("JSONL_PATH");
    const char* index_path = std::getenv("INDEX_PATH");

    const auto elapsed_ms = [](std::chrono::steady_clock::time_point from) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - from).count();
    };

    if (jsonl_path) {
        std::cout << "[engine] Building index from " << jsonl_path << "\n";

        const auto t0 = std::chrono::steady_clock::now();
        engine.build_from_jsonl(jsonl_path);
        const auto build_ms = elapsed_ms(t0);

        std::cout << "[engine] Built " << engine.num_docs() << " docs, "
                  << engine.num_terms() << " terms in " << build_ms << " ms\n";

        if (index_path) {
            const auto t1 = std::chrono::steady_clock::now();
            if (!engine.save(index_path)) {
                std::cerr << "[engine] Failed to save index to " << index_path << "\n";
                return 1;
            }
            const auto save_ms = elapsed_ms(t1);
            const std::uint64_t bytes = search::io::index_size_bytes(index_path);
            std::cout << "[engine] Saved index to " << index_path << " — "
                      << bytes << " bytes (" << (bytes / 1024) << " KiB) in "
                      << save_ms << " ms\n";
        }
        report_doc_store(engine);
    } else if (index_path) {
        // No JSONL needed: everything the build produced is on disk.
        std::cout << "[engine] Loading index from " << index_path << "\n";

        const auto t0 = std::chrono::steady_clock::now();
        if (!engine.load(index_path)) {
            std::cerr << "[engine] Failed to load index from " << index_path << "\n";
            return 1;
        }
        const auto load_ms = elapsed_ms(t0);
        const std::uint64_t bytes = search::io::index_size_bytes(index_path);

        std::cout << "[engine] Loaded " << engine.num_docs() << " docs, "
                  << engine.num_terms() << " terms from " << bytes << " bytes ("
                  << (bytes / 1024) << " KiB) in " << load_ms << " ms\n";
        report_doc_store(engine);
    } else {
        std::cout << "[engine] No JSONL_PATH or INDEX_PATH set — engine will be empty\n";
    }

    // BUILD_ONLY turns this binary into an index builder: everything above has
    // run, so the index is on disk, and we exit instead of serving. `make index`
    // uses it to seed the index volume as a discrete step.
    const char* build_only = std::getenv("BUILD_ONLY");
    if (build_only != nullptr && std::string(build_only) != "0") {
        std::cout << "[engine] BUILD_ONLY set — index is written, exiting without serving\n";
        return 0;
    }

    // ── Query cache ───────────────────────────────────────────────────────
    // QUERY_CACHE_CAPACITY sets the entry limit; 0 disables caching entirely,
    // which is how the benchmark measures uncached latency.
    if (const char* cap_env = std::getenv("QUERY_CACHE_CAPACITY")) {
        try {
            const long long cap = std::stoll(cap_env);
            if (cap < 0) throw std::out_of_range("negative");
            engine.set_cache_capacity(static_cast<std::size_t>(cap));
        } catch (const std::exception&) {
            std::cerr << "[engine] Ignoring QUERY_CACHE_CAPACITY=" << cap_env
                      << " (expected a non-negative integer)\n";
        }
    }
    {
        const std::size_t cap = engine.cache_stats().capacity;
        std::cout << "[engine] Query cache: "
                  << (cap == 0 ? "disabled" : std::to_string(cap) + " entries") << "\n";
    }

    // ── Configure Crow ────────────────────────────────────────────────────
    crow::SimpleApp app;

    // GET /stats — index and query-cache counters.
    //
    // Not part of Contract 2, which specifies POST /search only; this is
    // observability alongside the existing GET /health.
    CROW_ROUTE(app, "/stats").methods(crow::HTTPMethod::GET)(
        [&engine]() {
            const search::CacheStats cache = engine.cache_stats();
            const search::DocStore& store = engine.doc_store();

            json resp;
            resp["docs"]  = engine.num_docs();
            resp["terms"] = engine.num_terms();
            resp["doc_store"] = {
                {"offsets_bytes_in_ram", store.approx_ram_bytes()},
                {"backing", store.backing_path()},
            };
            resp["cache"] = {
                {"hits",      cache.hits},
                {"misses",    cache.misses},
                {"evictions", cache.evictions},
                {"hit_rate",  cache.hit_rate()},
                {"size",      cache.size},
                {"capacity",  cache.capacity},
            };

            auto r = crow::response(200, resp.dump());
            r.set_header("Content-Type", "application/json");
            return r;
        }
    );

    // GET /health — liveness probe
    CROW_ROUTE(app, "/health").methods(crow::HTTPMethod::GET)(
        []() {
            json resp;
            resp["status"] = "ok";
            auto r = crow::response(200, resp.dump());
            r.set_header("Content-Type", "application/json");
            return r;
        }
    );

    // POST /search — Contract 2
    CROW_ROUTE(app, "/search").methods(crow::HTTPMethod::POST)(
        [&engine](const crow::request& req) {
            json body;
            try {
                body = json::parse(req.body);
            } catch (const json::parse_error&) {
                json err;
                err["error"] = "Invalid JSON in request body";
                auto r = crow::response(400, err.dump());
                r.set_header("Content-Type", "application/json");
                return r;
            }

            if (!body.contains("query") || !body["query"].is_string()) {
                json err;
                err["error"] = "Missing or invalid 'query' field (string required)";
                auto r = crow::response(400, err.dump());
                r.set_header("Content-Type", "application/json");
                return r;
            }

            std::string query = body["query"].get<std::string>();
            int k = body.value("k", 10);

            if (k < 1) {
                json err;
                err["error"] = "'k' must be >= 1";
                auto r = crow::response(400, err.dump());
                r.set_header("Content-Type", "application/json");
                return r;
            }

            auto t0 = std::chrono::steady_clock::now();
            auto results = engine.search(query, k);
            auto t1 = std::chrono::steady_clock::now();

            int latency_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
            );

            json resp;
            resp["results"] = json::array();
            for (const auto& r : results) {
                resp["results"].push_back({
                    {"doc_id",  r.doc_id},
                    {"score",   r.score},
                    {"snippet", r.snippet},
                });
            }
            resp["latency_ms"] = latency_ms;

            auto response = crow::response(200, resp.dump());
            response.set_header("Content-Type", "application/json");
            return response;
        }
    );

    // ── Start server ──────────────────────────────────────────────────────
    int port = 8080;
    const char* port_env = std::getenv("ENGINE_PORT");
    if (port_env) {
        port = std::atoi(port_env);
    }

    std::cout << "[engine] Listening on port " << port << "\n";
    app.port(port).multithreaded().run();

    return 0;
}