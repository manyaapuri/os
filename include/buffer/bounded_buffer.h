#pragma once

#include "common/types.h"
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdint>

namespace dfs {

template <typename T>
class BoundedBuffer {
public:
    explicit BoundedBuffer(size_t capacity = 16)
        : capacity_(capacity), buffer_(capacity) {}

    ~BoundedBuffer() {
        close();
    }

    bool push(const T& item, int timeout_ms = 2000) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (is_closed_) return false;

        if (count_ == capacity_) {
            blocked_producers_count_++;
            bool space_available = cv_not_full_.wait_for(
                lock, std::chrono::milliseconds(timeout_ms),
                [this] { return count_ < capacity_ || is_closed_; }
            );
            blocked_producers_count_--;

            if (is_closed_ || !space_available) {
                return false;
            }
        }

        buffer_[tail_] = item;
        tail_ = (tail_ + 1) % capacity_;
        count_++;
        total_produced_++;

        double current_pct = (static_cast<double>(count_) / capacity_) * 100.0;
        if (current_pct > high_watermark_pct_) {
            high_watermark_pct_ = current_pct;
        }

        cv_not_empty_.notify_one();
        return true;
    }

    bool pop(T& item, int timeout_ms = 2000) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (count_ == 0) {
            if (is_closed_) return false;

            blocked_consumers_count_++;
            bool item_available = cv_not_empty_.wait_for(
                lock, std::chrono::milliseconds(timeout_ms),
                [this] { return count_ > 0 || is_closed_; }
            );
            blocked_consumers_count_--;

            if (!item_available || (count_ == 0 && is_closed_)) {
                return false;
            }
        }

        item = std::move(buffer_[head_]);
        head_ = (head_ + 1) % capacity_;
        count_--;
        total_consumed_++;

        cv_not_full_.notify_one();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            is_closed_ = true;
        }
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        is_closed_ = false;
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }

    BufferStats getStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        BufferStats stats;
        stats.capacity = capacity_;
        stats.current_occupancy = count_;
        stats.total_produced = total_produced_;
        stats.total_consumed = total_consumed_;
        stats.blocked_producers = blocked_producers_count_;
        stats.blocked_consumers = blocked_consumers_count_;
        stats.high_watermark_pct = high_watermark_pct_;
        return stats;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    size_t capacity() const {
        return capacity_;
    }

private:
    const size_t capacity_;
    std::vector<T> buffer_;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t count_ = 0;

    bool is_closed_ = false;
    uint64_t total_produced_ = 0;
    uint64_t total_consumed_ = 0;
    uint64_t blocked_producers_count_ = 0;
    uint64_t blocked_consumers_count_ = 0;
    double high_watermark_pct_ = 0.0;

    mutable std::mutex mutex_;
    std::condition_variable cv_not_full_;
    std::condition_variable cv_not_empty_;
};

// Represents a memory chunk transferred via bounded buffer
struct MemoryChunk {
    std::string file_path;
    int chunk_index = 0;
    std::vector<uint8_t> data;
    std::string checksum_sha256;
    bool is_last_chunk = false;
};

using ChunkBuffer = BoundedBuffer<MemoryChunk>;

} // namespace dfs
