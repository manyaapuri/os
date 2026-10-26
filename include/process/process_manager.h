#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>

namespace dfs {

struct ProcessState {
    int node_id = 0;
    std::string name;
    DWORD process_id = 0;
    HANDLE process_handle = NULL;
    HANDLE thread_handle = NULL;
    bool is_running = false;
    uint64_t memory_working_set_bytes = 0;
    double cpu_usage_pct = 0.0;
    int thread_count = 0;
    int port = 0;
    std::string storage_dir;
};

class ProcessManager {
public:
    ProcessManager();
    ~ProcessManager();

    // Launch a storage node child process natively via CreateProcessW
    bool launchStorageNode(int node_id, const std::string& exe_path, int port, const std::string& storage_dir);

    // Terminate a storage node process (graceful or forced)
    bool terminateProcess(int node_id, bool force = false);

    // Refresh state and check if child processes are still running
    void updateProcessStates();

    // Query process details
    std::vector<ProcessState> getAllProcessStates();
    bool isNodeProcessRunning(int node_id);

    // Gracefully terminate all child processes
    void shutdownAll();

    // System thread info
    struct ThreadPoolStats {
        int active_worker_threads = 0;
        int total_tasks_executed = 0;
    };
    ThreadPoolStats getThreadPoolStats() const;
    void recordTaskExecuted();
    void incrementWorkerThreads();
    void decrementWorkerThreads();

private:
    mutable std::mutex mutex_;
    std::map<int, ProcessState> managed_processes_;

    int active_workers_ = 0;
    int total_tasks_executed_ = 0;
};

} // namespace dfs
