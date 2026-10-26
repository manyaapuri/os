#include "sync/lock_manager.h"
#include <algorithm>
#include <functional>

namespace dfs {

LockManager::LockManager() = default;
LockManager::~LockManager() = default;

bool LockManager::acquireLock(const std::string& path, LockType type, const std::string& client_id, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    WaitingLockRequest req;
    req.request_id = client_id + "_" + std::to_string(now);
    req.resource_path = path;
    req.type = type;
    req.client_id = client_id;
    req.requested_at = now;
    req.timeout_ms = timeout_ms;

    waiting_queue_.push_back(req);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    bool acquired = cv_.wait_until(lock, deadline, [this, &path, type, &client_id] {
        auto& res = locks_[path];
        if (type == LockType::SHARED_READ) {
            // Can acquire shared read if no exclusive lock held
            return !res.exclusive_locked;
        } else {
            // Can acquire exclusive write if no readers and no exclusive lock held
            return !res.exclusive_locked && res.shared_readers == 0;
        }
    });

    // Remove from waiting queue
    for (auto it = waiting_queue_.begin(); it != waiting_queue_.end(); ++it) {
        if (it->resource_path == path && it->client_id == client_id) {
            waiting_queue_.erase(it);
            break;
        }
    }

    if (!acquired) {
        total_timeouts_++;
        return false;
    }

    auto& res = locks_[path];
    if (type == LockType::SHARED_READ) {
        res.shared_readers++;
        res.shared_holders.insert(client_id);
    } else {
        res.exclusive_locked = true;
        res.exclusive_holder = client_id;
    }
    res.acquired_at = now;
    total_acquired_++;

    return true;
}

bool LockManager::releaseLock(const std::string& path, const std::string& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = locks_.find(path);
    if (it == locks_.end()) return false;

    auto& res = it->second;
    bool released = false;

    if (res.exclusive_locked && res.exclusive_holder == client_id) {
        res.exclusive_locked = false;
        res.exclusive_holder.clear();
        released = true;
    } else if (res.shared_holders.find(client_id) != res.shared_holders.end()) {
        res.shared_holders.erase(client_id);
        if (res.shared_readers > 0) res.shared_readers--;
        released = true;
    }

    if (released) {
        if (!res.exclusive_locked && res.shared_readers == 0) {
            locks_.erase(it);
        }
        cv_.notify_all();
        return true;
    }
    return false;
}

bool LockManager::acquireMultipleLocks(const std::vector<std::string>& paths, LockType type, const std::string& client_id, int timeout_ms) {
    // OS Lock Hierarchy Rule: Sort all resource paths lexicographically.
    // This strictly eliminates the Circular Wait condition (Havender's algorithm).
    std::vector<std::string> sorted_paths = paths;
    std::sort(sorted_paths.begin(), sorted_paths.end());
    sorted_paths.erase(std::unique(sorted_paths.begin(), sorted_paths.end()), sorted_paths.end());

    std::vector<std::string> acquired_so_far;
    int per_lock_timeout = timeout_ms / static_cast<int>(sorted_paths.empty() ? 1 : sorted_paths.size());
    if (per_lock_timeout < 500) per_lock_timeout = 500;

    for (const auto& path : sorted_paths) {
        if (acquireLock(path, type, client_id, per_lock_timeout)) {
            acquired_so_far.push_back(path);
        } else {
            // Deadlock / Timeout recovery: Rollback all previously acquired locks in this set
            for (const auto& rollback_path : acquired_so_far) {
                releaseLock(rollback_path, client_id);
            }
            total_deadlocks_prevented_++;
            return false;
        }
    }
    return true;
}

void LockManager::releaseMultipleLocks(const std::vector<std::string>& paths, const std::string& client_id) {
    for (const auto& path : paths) {
        releaseLock(path, client_id);
    }
}

std::vector<LockInfo> LockManager::getActiveLocks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LockInfo> list;

    for (const auto& [path, res] : locks_) {
        if (res.exclusive_locked) {
            LockInfo info;
            info.resource_path = path;
            info.type = LockType::EXCLUSIVE_WRITE;
            info.client_id = res.exclusive_holder;
            info.acquired_at = res.acquired_at;
            info.expires_at = res.acquired_at + 30; // e.g. 30s lease
            list.push_back(info);
        }
        for (const auto& holder : res.shared_holders) {
            LockInfo info;
            info.resource_path = path;
            info.type = LockType::SHARED_READ;
            info.client_id = holder;
            info.acquired_at = res.acquired_at;
            info.expires_at = res.acquired_at + 30;
            list.push_back(info);
        }
    }
    return list;
}

std::vector<WaitingLockRequest> LockManager::getWaitingRequests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiting_queue_;
}

bool LockManager::detectDeadlockCycle(std::vector<std::string>& cycle_nodes) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Construct Wait-For Graph (WFG)
    // Directed edge: Client A -> Client B if A is waiting for a resource held by B
    std::map<std::string, std::set<std::string>> wfg;

    for (const auto& wait_req : waiting_queue_) {
        auto it = locks_.find(wait_req.resource_path);
        if (it != locks_.end()) {
            const auto& res = it->second;
            if (res.exclusive_locked && !res.exclusive_holder.empty()) {
                if (wait_req.client_id != res.exclusive_holder) {
                    wfg[wait_req.client_id].insert(res.exclusive_holder);
                }
            }
            for (const auto& reader : res.shared_holders) {
                if (wait_req.client_id != reader && wait_req.type == LockType::EXCLUSIVE_WRITE) {
                    wfg[wait_req.client_id].insert(reader);
                }
            }
        }
    }

    // Cycle detection via DFS
    std::map<std::string, int> visited; // 0=unvisited, 1=visiting, 2=visited
    std::vector<std::string> path;

    std::function<bool(const std::string&)> dfs = [&](const std::string& node) -> bool {
        visited[node] = 1;
        path.push_back(node);

        for (const auto& neighbor : wfg[node]) {
            if (visited[neighbor] == 1) {
                // Cycle found
                cycle_nodes.clear();
                auto start_it = std::find(path.begin(), path.end(), neighbor);
                if (start_it != path.end()) {
                    cycle_nodes.assign(start_it, path.end());
                } else {
                    cycle_nodes = path;
                }
                cycle_nodes.push_back(neighbor);
                return true;
            }
            if (visited[neighbor] == 0) {
                if (dfs(neighbor)) return true;
            }
        }

        visited[node] = 2;
        path.pop_back();
        return false;
    };

    for (const auto& [node, _] : wfg) {
        if (visited[node] == 0) {
            if (dfs(node)) return true;
        }
    }

    return false;
}

LockManager::Diagnostics LockManager::getDiagnostics() const {
    Diagnostics d;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        d.active_lock_count = locks_.size();
        d.waiting_request_count = waiting_queue_.size();
        d.total_acquired = total_acquired_;
        d.total_timeouts = total_timeouts_;
        d.total_deadlocks_prevented = total_deadlocks_prevented_;
    }
    d.has_deadlock_cycle = detectDeadlockCycle(d.deadlock_cycle);
    return d;
}

} // namespace dfs
