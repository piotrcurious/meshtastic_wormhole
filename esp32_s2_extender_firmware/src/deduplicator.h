#ifndef DEDUPLICATOR_H
#define DEDUPLICATOR_H

#include <Arduino.h>
#include <map>

struct CacheEntry {
    uint64_t source_id;
    uint32_t packet_id;
};

// Custom comparator for cache mapping
struct CacheComparator {
    bool operator()(const CacheEntry& a, const CacheEntry& b) const {
        if (a.source_id != b.source_id) {
            return a.source_id < b.source_id;
        }
        return a.packet_id < b.packet_id;
    }
};

class Deduplicator {
private:
    std::map<CacheEntry, uint32_t, CacheComparator> cache;
    uint32_t ttl_ms;

public:
    Deduplicator(uint32_t ttl_seconds = 120) : ttl_ms(ttl_seconds * 1000) {}

    bool is_duplicate(uint64_t source_id, uint32_t packet_id) {
        uint32_t now = millis();
        CacheEntry entry = {source_id, packet_id};

        cleanup();

        auto it = cache.find(entry);
        if (it != cache.end()) {
            if (now - it->second < ttl_ms) {
                return true;
            } else {
                cache.erase(it);
            }
        }

        cache[entry] = now;
        return false;
    }

    void cleanup() {
        uint32_t now = millis();
        for (auto it = cache.begin(); it != cache.end(); ) {
            if (now - it->second >= ttl_ms) {
                it = cache.erase(it);
            } else {
                ++it;
            }
        }
    }

    void clear() {
        cache.clear();
    }
};

#endif // DEDUPLICATOR_H
