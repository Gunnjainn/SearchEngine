// ---------------------------------------------------------------------------
// bench_query — measure query latency with and without the result cache.
//
// Contract 2's `latency_ms` is an integer and reports 0-2 ms on a corpus this
// size, which cannot express a percentile. This measures in microseconds
// in-process instead, so "p95 drops on repeats" is a number rather than a claim.
//
//   bench_query [corpus.jsonl] [repeats]
//
// Defaults to eval/corpus.frozen.jsonl so runs stay comparable over time.
// Not a unit test: it reports, it does not assert.
// ---------------------------------------------------------------------------

#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Queries chosen to span the work the engine actually does: single common term,
// single rare term, multi-term AND that matches, multi-term AND that matches
// nothing, and a stopword-only query.
const std::vector<std::string> kQueries = {
    "python",         "rust",           "compiler",       "database",
    "javascript",     "security",       "open source",    "machine learning",
    "rust compiler",  "web browser",    "the and of",     "zzzznotaterm",
    "api",            "linux kernel",   "startup",        "model",
};

double percentile(std::vector<double> samples, double p) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    // Nearest-rank: the smallest sample at or above the p-th position.
    const std::size_t rank =
        static_cast<std::size_t>(std::ceil(p * static_cast<double>(samples.size())));
    const std::size_t idx = (rank == 0) ? 0 : std::min(rank - 1, samples.size() - 1);
    return samples[idx];
}

double mean(const std::vector<double>& samples) {
    if (samples.empty()) return 0.0;
    return std::accumulate(samples.begin(), samples.end(), 0.0)
         / static_cast<double>(samples.size());
}

// Microseconds for one search() call.
double time_one(const Engine& engine, const std::string& query, int k) {
    const auto t0 = Clock::now();
    const auto results = engine.search(query, k);
    const auto t1 = Clock::now();
    // Keep the optimiser from discarding the call.
    if (results.size() == 0xFFFFFFFF) std::cout << "";
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

void report(const char* label, const std::vector<double>& us) {
    std::printf("  %-22s n=%-6zu p50=%9.1f  p95=%9.1f  p99=%9.1f  mean=%9.1f\n",
                label, us.size(), percentile(us, 0.50), percentile(us, 0.95),
                percentile(us, 0.99), mean(us));
}

}  // namespace

int main(int argc, char** argv) {
    const std::string corpus = (argc > 1) ? argv[1] : "eval/corpus.frozen.jsonl";
    const int repeats = (argc > 2) ? std::atoi(argv[2]) : 40;
    constexpr int kTopK = 10;

    Engine engine;
    engine.build_from_jsonl(corpus);
    if (engine.num_docs() == 0) {
        std::cerr << "bench_query: no documents indexed from " << corpus << "\n";
        return 1;
    }

    std::printf("\ncorpus   %s\n", corpus.c_str());
    std::printf("docs     %d\nterms    %zu\nqueries  %zu x %d repeats, k=%d\n\n",
                engine.num_docs(), engine.num_terms(), kQueries.size(), repeats, kTopK);

    // ── Uncached: capacity 0, so every call does the full five stages ──
    engine.set_cache_capacity(0);
    std::vector<double> cold;
    cold.reserve(kQueries.size() * static_cast<std::size_t>(repeats));
    for (int r = 0; r < repeats; ++r) {
        for (const std::string& q : kQueries) cold.push_back(time_one(engine, q, kTopK));
    }

    // ── Cached: one warm-up pass to populate, then measure the repeats ──
    engine.set_cache_capacity(search::QueryCache::kDefaultCapacity);
    for (const std::string& q : kQueries) time_one(engine, q, kTopK);  // populate

    const auto before = engine.cache_stats();
    std::vector<double> warm;
    warm.reserve(kQueries.size() * static_cast<std::size_t>(repeats));
    for (int r = 0; r < repeats; ++r) {
        for (const std::string& q : kQueries) warm.push_back(time_one(engine, q, kTopK));
    }
    const auto after = engine.cache_stats();

    std::printf("latency, microseconds\n");
    report("cache disabled", cold);
    report("cache warm", warm);

    const double p95_cold = percentile(cold, 0.95);
    const double p95_warm = percentile(warm, 0.95);
    const double p50_cold = percentile(cold, 0.50);
    const double p50_warm = percentile(warm, 0.50);

    std::printf("\nspeedup on repeats\n");
    std::printf("  p50  %.1f us -> %.1f us   (%.1fx faster)\n",
                p50_cold, p50_warm, (p50_warm > 0.0) ? p50_cold / p50_warm : 0.0);
    std::printf("  p95  %.1f us -> %.1f us   (%.1fx faster)\n",
                p95_cold, p95_warm, (p95_warm > 0.0) ? p95_cold / p95_warm : 0.0);

    // Count against lookups that actually reached the cache, not against timed
    // calls: a stopword-only query returns from search() before the cache is
    // consulted, so including it would understate the hit rate.
    const std::uint64_t measured_hits = after.hits - before.hits;
    const std::uint64_t measured_lookups =
        (after.hits + after.misses) - (before.hits + before.misses);
    const std::size_t skipped = warm.size() - static_cast<std::size_t>(measured_lookups);

    std::printf("\ncache over the measured phase\n");
    std::printf("  %llu hits of %llu lookups, hit rate %.1f%%, size %zu of %zu\n",
                static_cast<unsigned long long>(measured_hits),
                static_cast<unsigned long long>(measured_lookups),
                (measured_lookups == 0) ? 0.0
                    : 100.0 * static_cast<double>(measured_hits)
                            / static_cast<double>(measured_lookups),
                after.size, after.capacity);
    if (skipped > 0) {
        std::printf("  %zu calls never reached the cache (no terms after tokenizing)\n", skipped);
    }
    std::printf("\n");

    return 0;
}
