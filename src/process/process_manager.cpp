#include "process/process_manager.h"
#include "common/logger.h"
#include <psapi.h>
#include <sstream>

namespace dfs {

ProcessManager::ProcessManager() = default;

ProcessManager::~ProcessManager() {
    shutdownAll();
}

bool ProcessManager::launchStorageNode(int node_id, const std::string& exe_path, int port, const std::string& storage_dir) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if already running
    auto it = managed_processes_.find(node_id);
    if (it != managed_processes_.end() && it->second.is_running) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(it->second.process_handle, &exit_code) && exit_code == STILL_ACTIVE) {
            LOG_WARN("ProcessManager", "Node " + std::to_string(node_id) + " is already running with PID " +
                     std::to_string(it->second.process_id));
            return true;
        }
    }

    // Build command line: dfs_storage_node.exe --id <node_id> --port <port> --dir <storage_dir>
    std::ostringstream cmd_ss;
    cmd_ss << "\"" << exe_path << "\" --id " << node_id << " --port " << port << " --dir \"" << storage_dir << "\"";
    std::string cmd_str = cmd_ss.str();

    std::wstring wcmd(cmd_str.begin(), cmd_str.end());

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    // Launch process using Windows CreateProcessW
    BOOL ok = CreateProcessW(
        NULL,
        &wcmd[0],
        NULL,
        NULL,
        FALSE,
        CREATE_NEW_CONSOLE, // Launch in independent console window or background
        NULL,
        NULL,
        &si,
        &pi
    );

    if (!ok) {
        DWORD err = GetLastError();
        LOG_ERROR("ProcessManager", "CreateProcessW failed for node " + std::to_string(node_id) +
                  " with error: " + std::to_string(err) + " | Command: " + cmd_str);
        return false;
    }

    ProcessState state;
    state.node_id = node_id;
    state.name = "StorageNode_" + std::to_string(node_id);
    state.process_id = pi.dwProcessId;
    state.process_handle = pi.hProcess;
    state.thread_handle = pi.hThread;
    state.is_running = true;
    state.port = port;
    state.storage_dir = storage_dir;
    state.thread_count = 1;

    managed_processes_[node_id] = state;
    LOG_INFO("ProcessManager", "Successfully spawned Storage Node " + std::to_string(node_id) +
             " [PID: " + std::to_string(pi.dwProcessId) + "] on port " + std::to_string(port));
    return true;
}

bool ProcessManager::terminateProcess(int node_id, bool force) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = managed_processes_.find(node_id);
    if (it == managed_processes_.end() || !it->second.is_running) {
        return false;
    }

    HANDLE hProc = it->second.process_handle;
    if (hProc == NULL || hProc == INVALID_HANDLE_VALUE) {
        it->second.is_running = false;
        return true;
    }

    if (force) {
        TerminateProcess(hProc, 1);
    } else {
        // Allow up to 1 second for graceful exit before terminating
        if (WaitForSingleObject(hProc, 1000) != WAIT_OBJECT_0) {
            TerminateProcess(hProc, 1);
        }
    }

    CloseHandle(hProc);
    if (it->second.thread_handle) {
        CloseHandle(it->second.thread_handle);
    }

    it->second.is_running = false;
    it->second.process_handle = NULL;
    it->second.thread_handle = NULL;

    LOG_INFO("ProcessManager", "Terminated Storage Node " + std::to_string(node_id) +
             " [PID: " + std::to_string(it->second.process_id) + "]");
    return true;
}

void ProcessManager::updateProcessStates() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, state] : managed_processes_) {
        if (!state.is_running || state.process_handle == NULL) continue;

        DWORD exit_code = 0;
        if (GetExitCodeProcess(state.process_handle, &exit_code)) {
            if (exit_code != STILL_ACTIVE) {
                state.is_running = false;
                CloseHandle(state.process_handle);
                state.process_handle = NULL;
                continue;
            }
        }

        // Query memory usage
        PROCESS_MEMORY_COUNTERS pmc{};
        if (GetProcessMemoryInfo(state.process_handle, &pmc, sizeof(pmc))) {
            state.memory_working_set_bytes = pmc.WorkingSetSize;
        }
    }
}

std::vector<ProcessState> ProcessManager::getAllProcessStates() {
    updateProcessStates();
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProcessState> list;
    for (const auto& [_, state] : managed_processes_) {
        list.push_back(state);
    }
    return list;
}

bool ProcessManager::isNodeProcessRunning(int node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = managed_processes_.find(node_id);
    if (it == managed_processes_.end()) return false;
    return it->second.is_running;
}

void ProcessManager::shutdownAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, state] : managed_processes_) {
        if (state.is_running && state.process_handle != NULL) {
            TerminateProcess(state.process_handle, 0);
            CloseHandle(state.process_handle);
            if (state.thread_handle) CloseHandle(state.thread_handle);
            state.is_running = false;
            state.process_handle = NULL;
        }
    }
    managed_processes_.clear();
}

ProcessManager::ThreadPoolStats ProcessManager::getThreadPoolStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ThreadPoolStats s;
    s.active_worker_threads = active_workers_;
    s.total_tasks_executed = total_tasks_executed_;
    return s;
}

void ProcessManager::recordTaskExecuted() {
    std::lock_guard<std::mutex> lock(mutex_);
    total_tasks_executed_++;
}

void ProcessManager::incrementWorkerThreads() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_workers_++;
}

void ProcessManager::decrementWorkerThreads() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_workers_ > 0) active_workers_--;
}

} // namespace dfs
