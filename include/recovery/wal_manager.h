#pragma once

#include "common/types.h"
#include "metadata/metadata_manager.h"
#include <string>
#include <vector>
#include <memory>

namespace dfs {

class WalManager {
public:
    explicit WalManager(std::shared_ptr<MetadataManager> metadata_mgr);
    ~WalManager();

    // Transaction lifecycle
    std::string beginTransaction(const std::string& op, const std::string& file_path, const std::string& payload = "");
    bool commitTransaction(const std::string& tx_id);
    bool abortTransaction(const std::string& tx_id);

    // Startup Crash Recovery
    struct RecoveryReport {
        size_t pending_operations_found = 0;
        size_t operations_rolled_back = 0;
        size_t operations_recovered = 0;
        std::vector<std::string> recovery_messages;
    };
    RecoveryReport runCrashRecovery();

    // Atomic file update utilities
    static bool atomicWriteFile(const std::string& target_path, const uint8_t* data, size_t len, std::string& out_sha256);
    static bool verifyFileIntegrity(const std::string& file_path, const std::string& expected_sha256);

    std::vector<WalLogEntry> getRecentLogs(int limit = 50);

private:
    std::shared_ptr<MetadataManager> metadata_;
};

} // namespace dfs
