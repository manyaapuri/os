#pragma once

#include "common/types.h"
#include <string>
#include <vector>
#include <list>
#include <unordered_map>
#include <mutex>
#include <cstdint>

namespace dfs {

class LruFileCache {
public:
    struct CacheEntry {
        std::string key;            // Usually file path or chunk_id
        std::vector<uint8_t> data;
        std::string checksum_sha256;
        int version = 1;
        int64_t cached_at = 0;
    };

    explicit LruFileCache(size_t capacity_items = 64)
        : capacity_(capacity_items) {}

    bool get(const std::string& key, std::vector<uint8_t>& out_data, std::string* out_checksum = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            miss_count_++;
            return false;
        }

        // Move to front of LRU list (most recently used)
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);

        out_data = it->second->data;
        if (out_checksum) {
            *out_checksum = it->second->checksum_sha256;
        }
        hit_count_++;
        return true;
    }

    void put(const std::string& key, const std::vector<uint8_t>& data, const std::string& checksum, int version = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            // Update existing entry
            it->second->data = data;
            it->second->checksum_sha256 = checksum;
            it->second->version = version;
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
            return;
        }

        // Check capacity and evict least recently used (back of list)
        if (lru_list_.size() >= capacity_) {
            auto lru_it = --lru_list_.end();
            map_.erase(lru_it->key);
            lru_list_.pop_back();
            eviction_count_++;
        }

        CacheEntry entry;
        entry.key = key;
        entry.data = data;
        entry.checksum_sha256 = checksum;
        entry.version = version;

        lru_list_.push_front(std::move(entry));
        map_[key] = lru_list_.begin();
    }

    void invalidate(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            lru_list_.erase(it->second);
            map_.erase(it);
            invalidation_count_++;
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        lru_list_.clear();
        map_.clear();
    }

    CacheStats getStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        CacheStats s;
        s.capacity_items = capacity_;
        s.current_items = lru_list_.size();
        s.hit_count = hit_count_;
        s.miss_count = miss_count_;
        s.eviction_count = eviction_count_;
        s.invalidation_count = invalidation_count_;

        uint64_t total = hit_count_ + miss_count_;
        s.hit_ratio_pct = (total > 0) ? (static_cast<double>(hit_count_) / total * 100.0) : 0.0;
        return s;
    }

    std::vector<std::string> getCachedKeys() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> keys;
        for (const auto& item : lru_list_) {
            keys.push_back(item.key);
        }
        return keys;
    }

private:
    const size_t capacity_;
    mutable std::mutex mutex_;

    std::list<CacheEntry> lru_list_;
    std::unordered_map<std::string, std::list<CacheEntry>::iterator> map_;

    uint64_t hit_count_ = 0;
    uint64_t miss_count_ = 0;
    uint64_t eviction_count_ = 0;
    uint64_t invalidation_count_ = 0;
};

} // namespace dfs
