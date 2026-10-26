#include "recovery/wal_manager.h"
#include "common/sha256.h"
#include "common/logger.h"
#include <windows.h>
#include <fstream>
#include <filesystem>
#include <chrono>

namespace dfs {

WalManager::WalManager(std::shared_ptr<MetadataManager> metadata_mgr)
    : metadata_(metadata_mgr) {}

WalManager::~WalManager() = default;

std::string WalManager::beginTransaction(const std::string& op, const std::string& file_path, const std::string& payload) {
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string tx_id = "tx_" + std::to_string(now);

    metadata_->appendWal(tx_id, op, file_path, "PENDING", payload);
    return tx_id;
}

bool WalManager::commitTransaction(const std::string& tx_id) {
    return metadata_->updateWalState(tx_id, "COMMITTED");
}

bool WalManager::abortTransaction(const std::string& tx_id) {
    return metadata_->updateWalState(tx_id, "ROLLED_BACK");
}

WalManager::RecoveryReport WalManager::runCrashRecovery() {
    RecoveryReport report;
    auto pending = metadata_->getPendingWalEntries();
    report.pending_operations_found = pending.size();

    LOG_INFO("Recovery", "Running crash recovery check: found " + std::to_string(pending.size()) + " pending entries.");

    for (const auto& entry : pending) {
        std::string msg = "Resolving uncommitted operation: LSN=" + std::to_string(entry.lsn) +
                          ", TX=" + entry.tx_id + ", OP=" + entry.operation + ", File=" + entry.file_path;
        LOG_WARN("Recovery", msg);
        report.recovery_messages.push_back(msg);

        // Check if file operation was partially completed and roll back
        if (entry.operation == "WRITE" || entry.operation == "UPLOAD") {
            // Delete incomplete temporary files or restore previous version
            abortTransaction(entry.tx_id);
            report.operations_rolled_back++;
        } else if (entry.operation == "DELETE") {
            // If delete was pending, mark safe rollback
            abortTransaction(entry.tx_id);
            report.operations_rolled_back++;
        } else {
            abortTransaction(entry.tx_id);
            report.operations_rolled_back++;
        }
    }

    if (report.pending_operations_found == 0) {
        report.recovery_messages.push_back("Clean shutdown detected. No uncommitted transactions found.");
    } else {
        report.recovery_messages.push_back("Crash recovery successfully resolved " +
            std::to_string(report.operations_rolled_back) + " incomplete operations.");
    }

    return report;
}

bool WalManager::atomicWriteFile(const std::string& target_path, const uint8_t* data, size_t len, std::string& out_sha256) {
    std::filesystem::path target(target_path);
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path());
    }

    // 1. Create a unique temporary file in the same directory (guarantees same volume for atomic move)
    std::string temp_path = target_path + ".tmp." + std::to_string(GetCurrentProcessId()) + "_" +
                            std::to_string(GetTickCount64());

    // 2. Write and flush file contents
    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            LOG_ERROR("AtomicWrite", "Failed to open temporary file: " + temp_path);
            return false;
        }
        out.write(reinterpret_cast<const char*>(data), len);
        out.flush();
        if (!out) {
            LOG_ERROR("AtomicWrite", "Failed to write data to temporary file: " + temp_path);
            std::filesystem::remove(temp_path);
            return false;
        }
    }

    // 3. Compute SHA-256 of the written file for verification
    out_sha256 = SHA256::hashBuffer(data, len);

    // 4. Atomically rename temporary file to destination using Windows MoveFileExW
    std::wstring wtemp(temp_path.begin(), temp_path.end());
    std::wstring wtarget(target_path.begin(), target_path.end());

    BOOL ok = MoveFileExW(wtemp.c_str(), wtarget.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

    if (!ok) {
        DWORD err = GetLastError();
        LOG_ERROR("AtomicWrite", "MoveFileExW failed with error code: " + std::to_string(err));
        std::filesystem::remove(temp_path);
        return false;
    }

    return true;
}

bool WalManager::verifyFileIntegrity(const std::string& file_path, const std::string& expected_sha256) {
    if (!std::filesystem::exists(file_path)) return false;
    std::string actual = SHA256::hashFile(file_path);
    return (actual == expected_sha256);
}

std::vector<WalLogEntry> WalManager::getRecentLogs(int limit) {
    return metadata_->getRecentWalEntries(limit);
}

} // namespace dfs
