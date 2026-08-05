#include "query_cache.h"

#include <algorithm>
#include <utility>

namespace search {

QueryCache::QueryCache(std::size_t capacity) : cap(capacity) {}

bool QueryCache::get(const std::string& key, std::vector<Result>& out) {
    std::lock_guard<std::mutex> lock(mutex);

    auto it = index.find(key);
    if (it == index.end()) {
        ++misses;
        return false;
    }

    // Promote to most-recently-used. splice moves the node without copying it
    // and, importantly, leaves the iterator in `index` valid.
    lru.splice(lru.begin(), lru, it->second);
    out = it->second->results;
    ++hits;
    return true;
}

void QueryCache::put(const std::string& key, std::vector<Result> results) {
    std::lock_guard<std::mutex> lock(mutex);

    if (cap == 0) return;  // caching disabled

    auto it = index.find(key);
    if (it != index.end()) {
        // Replace in place and promote; the key already occupies a slot.
        it->second->results = std::move(results);
        lru.splice(lru.begin(), lru, it->second);
        return;
    }

    lru.push_front(Entry{key, std::move(results)});
    index[key] = lru.begin();
    evict_to_capacity();
}

void QueryCache::evict_to_capacity() {
    while (lru.size() > cap) {
        index.erase(lru.back().key);
        lru.pop_back();
        ++evictions;
    }
}

void QueryCache::clear() {
    std::lock_guard<std::mutex> lock(mutex);
    // Counters survive: they are lifetime telemetry, not a property of the
    // current contents.
    lru.clear();
    index.clear();
}

CacheStats QueryCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex);
    CacheStats s;
    s.hits      = hits;
    s.misses    = misses;
    s.evictions = evictions;
    s.size      = lru.size();
    s.capacity  = cap;
    return s;
}

std::size_t QueryCache::capacity() const {
    std::lock_guard<std::mutex> lock(mutex);
    return cap;
}

void QueryCache::set_capacity(std::size_t capacity) {
    std::lock_guard<std::mutex> lock(mutex);
    cap = capacity;
    if (cap == 0) {
        // Disabling the cache has to drop what it already holds, otherwise
        // stale entries would keep being served from a "disabled" cache.
        index.clear();
        lru.clear();
        return;
    }
    evict_to_capacity();
}

std::string cache_key(QueryMode mode, const std::vector<std::string>& terms, int k) {
    // Unit Separator cannot appear inside a term: the tokenizer classifies every
    // ASCII control character as a separator, so it never survives into a token.
    constexpr char kSep = '\x1f';

    std::vector<std::string> sorted = terms;
    std::sort(sorted.begin(), sorted.end());  // duplicates deliberately kept

    std::string key;
    key.reserve(sorted.size() * 8 + 8);
    key += (mode == QueryMode::And) ? 'A' : 'O';
    key += kSep;
    key += std::to_string(k);
    for (const std::string& term : sorted) {
        key += kSep;
        key += term;
    }
    return key;
}

}  // namespace search
