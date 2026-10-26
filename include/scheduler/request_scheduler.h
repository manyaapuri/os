#pragma once

#include "common/types.h"
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <vector>
#include <memory>

namespace dfs {

class RequestScheduler {
public:
    explicit RequestScheduler(SchedulingAlgorithm algo = SchedulingAlgorithm::FCFS);
    ~RequestScheduler();

    void setAlgorithm(SchedulingAlgorithm algo);
    SchedulingAlgorithm getAlgorithm() const;

    // Enqueue request
    void enqueue(ScheduledRequest req);

    // Dequeue next request according to current algorithm (blocks until request available or timeout)
    bool dequeue(ScheduledRequest& req, int timeout_ms = 1000);

    // Mark completion of a request to update metrics
    void recordCompletion(const ScheduledRequest& req);

    // Statistics
    SchedulerStats getStats() const;
    std::vector<ScheduledRequest> getQueueSnapshot() const;
    void resetStats();

private:
    SchedulingAlgorithm algorithm_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::deque<ScheduledRequest> queue_;
    std::deque<ScheduledRequest> recent_completed_;

    // Metric accumulators
    uint64_t total_serviced_ = 0;
    double total_wait_time_ms_ = 0.0;
    double total_response_time_ms_ = 0.0;
    double total_turnaround_time_ms_ = 0.0;
    int64_t start_time_epoch_ms_ = 0;
};

} // namespace dfs
