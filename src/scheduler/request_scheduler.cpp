#include "scheduler/request_scheduler.h"
#include <algorithm>

namespace dfs {

RequestScheduler::RequestScheduler(SchedulingAlgorithm algo)
    : algorithm_(algo) {
    start_time_epoch_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

RequestScheduler::~RequestScheduler() = default;

void RequestScheduler::setAlgorithm(SchedulingAlgorithm algo) {
    std::lock_guard<std::mutex> lock(mutex_);
    algorithm_ = algo;
}

SchedulingAlgorithm RequestScheduler::getAlgorithm() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return algorithm_;
}

void RequestScheduler::enqueue(ScheduledRequest req) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    if (req.enqueue_time_ms == 0) {
        req.enqueue_time_ms = now;
    }
    if (req.remaining_quantum_ms <= 0) {
        req.remaining_quantum_ms = req.time_quantum_ms;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(req);
    }
    cv_.notify_one();
}

bool RequestScheduler::dequeue(ScheduledRequest& req, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (queue_.empty()) {
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !queue_.empty(); })) {
            return false;
        }
    }

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    size_t chosen_idx = 0;

    switch (algorithm_) {
        case SchedulingAlgorithm::FCFS: {
            // Pick head of queue
            chosen_idx = 0;
            break;
        }

        case SchedulingAlgorithm::PRIORITY: {
            // Find highest priority request, breaking ties by earliest enqueue_time
            // Apply priority aging: older requests gain virtual priority boost
            int best_score = -99999;
            for (size_t i = 0; i < queue_.size(); ++i) {
                int64_t wait_time = now - queue_[i].enqueue_time_ms;
                int age_boost = static_cast<int>(wait_time / 200); // +1 priority per 200ms waited
                int score = (static_cast<int>(queue_[i].priority) * 10) + age_boost;
                if (score > best_score) {
                    best_score = score;
                    chosen_idx = i;
                }
            }
            break;
        }

        case SchedulingAlgorithm::ROUND_ROBIN: {
            // Pick front, simulate time slice execution
            chosen_idx = 0;
            break;
        }
    }

    req = queue_[chosen_idx];
    queue_.erase(queue_.begin() + chosen_idx);

    if (req.start_time_ms == 0) {
        req.start_time_ms = now;
    }

    return true;
}

void RequestScheduler::recordCompletion(const ScheduledRequest& req) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    ScheduledRequest completed = req;
    completed.finish_time_ms = now;

    int64_t wait_time = completed.start_time_ms - completed.enqueue_time_ms;
    if (wait_time < 0) wait_time = 0;

    int64_t response_time = wait_time;
    int64_t turnaround_time = completed.finish_time_ms - completed.enqueue_time_ms;
    if (turnaround_time < 0) turnaround_time = 0;

    std::lock_guard<std::mutex> lock(mutex_);
    total_serviced_++;
    total_wait_time_ms_ += static_cast<double>(wait_time);
    total_response_time_ms_ += static_cast<double>(response_time);
    total_turnaround_time_ms_ += static_cast<double>(turnaround_time);

    recent_completed_.push_back(completed);
    if (recent_completed_.size() > 50) {
        recent_completed_.pop_front();
    }
}

SchedulerStats RequestScheduler::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SchedulerStats s;
    s.current_algorithm = algorithm_;
    s.total_requests_serviced = total_serviced_;
    s.queue_depth = queue_.size();

    if (total_serviced_ > 0) {
        s.avg_waiting_time_ms = total_wait_time_ms_ / total_serviced_;
        s.avg_response_time_ms = total_response_time_ms_ / total_serviced_;
        s.avg_turnaround_time_ms = total_turnaround_time_ms_ / total_serviced_;
    }

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    double elapsed_sec = (now - start_time_epoch_ms_) / 1000.0;
    if (elapsed_sec > 0.0) {
        s.throughput_ops_sec = total_serviced_ / elapsed_sec;
    }

    return s;
}

std::vector<ScheduledRequest> RequestScheduler::getQueueSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<ScheduledRequest>(queue_.begin(), queue_.end());
}

void RequestScheduler::resetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    total_serviced_ = 0;
    total_wait_time_ms_ = 0.0;
    total_response_time_ms_ = 0.0;
    total_turnaround_time_ms_ = 0.0;
    start_time_epoch_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace dfs
