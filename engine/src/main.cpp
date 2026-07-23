// ---------------------------------------------------------------------------
// Search Engine — Crow HTTP Server
//
// Serves POST /search (Contract 2) and GET /health.
// Delegates search logic to the Engine class.
// ---------------------------------------------------------------------------

#include "engine.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "crow.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

int main() {
    // ── Initialise engine ─────────────────────────────────────────────────
    Engine engine;

    const char* jsonl_path = std::getenv("JSONL_PATH");
    const char* index_path = std::getenv("INDEX_PATH");
    
    if (jsonl_path) {
        std::cout << "[engine] Building index from " << jsonl_path << "\n";
        engine.build_from_jsonl(jsonl_path);
        if (index_path) {
            engine.save(index_path);
        }
    } else if (index_path) {
        std::cout << "[engine] Loading index from " << index_path << "\n";
        engine.load(index_path);
    } else {
        std::cout << "[engine] No JSONL_PATH or INDEX_PATH set — engine will be empty\n";
    }

    // ── Configure Crow ────────────────────────────────────────────────────
    crow::SimpleApp app;

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
            // ── Parse request body ────────────────────────────────────
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

            // ── Execute search ────────────────────────────────────────
            auto t0 = std::chrono::steady_clock::now();
            auto results = engine.search(query, k);
            auto t1 = std::chrono::steady_clock::now();

            int latency_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
            );

            // ── Build response ────────────────────────────────────────
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
