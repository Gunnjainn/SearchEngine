#pragma once
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "query.h"   // QueryMode
#include "result.h"  // Result

namespace search {

// ---------------------------------------------------------------------------
// CacheStats — lifetime counters, plus the current occupancy.
//
// hits/misses/evictions are cumulative for the life of the process and are NOT
// reset by clear(): they describe what the cache has done, not what it holds.
// ---------------------------------------------------------------------------
struct CacheStats {
    std::uint64_t hits      = 0;
    std::uint64_t misses    = 0;
    std::uint64_t evictions = 0;
    std::size_t   size      = 0;  // entries currently held
    std::size_t   capacity  = 0;  // 0 means caching is disabled

    // Fraction of lookups served from the cache; 0.0 before any lookup.
    double hit_rate() const {
        const std::uint64_t total = hits + misses;
        return (total == 0) ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
    }
};

// ---------------------------------------------------------------------------
// QueryCache — a size-bounded, thread-safe LRU cache of search results.
//
// Structure is the standard one: a list in recency order plus a map from key to
// list position, which makes lookup, promotion and eviction all O(1).
//
// One mutex guards everything, and get() takes it exclusively even though it
// reads: an LRU lookup *promotes* the entry it finds, so there is no read-only
// path to optimise. A shared_mutex would buy nothing here.
//
// Bounded by entry count rather than bytes. Each entry holds up to k Results
// with their snippets, so the default 256 entries at k=10 is on the order of
// half a megabyte — small next to the index, and predictable.
// ---------------------------------------------------------------------------
class QueryCache {
public:
    static constexpr std::size_t kDefaultCapacity = 256;

    explicit QueryCache(std::size_t capacity = kDefaultCapacity);

    // On a hit: copies the stored results into `out`, promotes the entry to
    // most-recently-used, and returns true. On a miss: leaves `out` alone.
    bool get(const std::string& key, std::vector<Result>& out);

    // Inserts or replaces, then evicts the least-recently-used entries until
    // the size is within capacity. A capacity of 0 stores nothing.
    void put(const std::string& key, std::vector<Result> results);

    // Drops every entry. Must be called whenever the index changes, or the
    // cache will keep serving results computed against the old one.
    void clear();

    CacheStats  stats() const;
    std::size_t capacity() const;

    // Re-bounds the cache, evicting immediately if it now holds too much.
    void set_capacity(std::size_t capacity);

private:
    struct Entry {
        std::string         key;  // kept so eviction can erase from the map
        std::vector<Result> results;
    };

    // Caller must hold the lock.
    void evict_to_capacity();

    mutable std::mutex mutex;

    std::list<Entry> lru;  // front is most recently used
    std::unordered_map<std::string, std::list<Entry>::iterator> index;

    std::size_t   cap;
    std::uint64_t hits      = 0;
    std::uint64_t misses    = 0;
    std::uint64_t evictions = 0;
};

// ---------------------------------------------------------------------------
// cache_key — the identity of a query result.
//
// Built from the *parsed* terms rather than the raw string, because the terms
// are what determine the result: "Running the Compilers!" and "run compil"
// tokenize identically and should share one entry.
//
// Terms are sorted, so "compiler database" and "database compiler" also share
// one. That is safe rather than merely convenient: BM25 sums independent
// per-term contributions, snippet windows treat the terms as a set, and the
// top-k tie-break is on doc_id — none of them depend on query order. Duplicates
// are preserved, because a term written twice is scored twice.
//
// `mode` and `k` are part of the key: both change the result.
// ---------------------------------------------------------------------------
std::string cache_key(QueryMode mode, const std::vector<std::string>& terms, int k);

}  // namespace search
