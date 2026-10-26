#pragma once

#include "common/types.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace dfs {

class LockManager {
public:
    LockManager();
    ~LockManager();

    // Acquire lock with timeout (returns true on success, false on timeout/failure)
    bool acquireLock(const std::string& path, LockType type, const std::string& client_id, int timeout_ms = 3000);

    // Release lock
    bool releaseLock(const std::string& path, const std::string& client_id);

    // Hierarchical multi-lock acquisition to prevent deadlocks (acquires in canonical alphabetical order)
    bool acquireMultipleLocks(const std::vector<std::string>& paths, LockType type, const std::string& client_id, int timeout_ms = 5000);
    void releaseMultipleLocks(const std::vector<std::string>& paths, const std::string& client_id);

    // Diagnostics & State
    std::vector<LockInfo> getActiveLocks() const;
    std::vector<WaitingLockRequest> getWaitingRequests() const;

    // Deadlock Detection via Wait-For Graph (WFG) cycle detection
    bool detectDeadlockCycle(std::vector<std::string>& cycle_nodes) const;

    // Diagnostics summary
    struct Diagnostics {
        size_t active_lock_count = 0;
        size_t waiting_request_count = 0;
        uint64_t total_acquired = 0;
        uint64_t total_timeouts = 0;
        uint64_t total_deadlocks_prevented = 0;
        bool has_deadlock_cycle = false;
        std::vector<std::string> deadlock_cycle;
    };
    Diagnostics getDiagnostics() const;

private:
    struct ResourceLockState {
        int shared_readers = 0;
        bool exclusive_locked = false;
        std::string exclusive_holder;
        std::set<std::string> shared_holders;
        int64_t acquired_at = 0;
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::map<std::string, ResourceLockState> locks_;
    std::vector<WaitingLockRequest> waiting_queue_;

    uint64_t total_acquired_ = 0;
    uint64_t total_timeouts_ = 0;
    uint64_t total_deadlocks_prevented_ = 0;
};

} // namespace dfs
