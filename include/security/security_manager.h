#pragma once

#include "common/types.h"
#include "metadata/metadata_manager.h"
#include <string>
#include <memory>

namespace dfs {

class SecurityManager {
public:
    explicit SecurityManager(std::shared_ptr<MetadataManager> metadata_mgr);
    ~SecurityManager();

    // Path sanitization & traversal prevention
    static bool sanitizePath(const std::string& raw_path, std::string& clean_path);
    static bool isPathSafe(const std::string& path);

    // Authentication & authorization
    bool authenticate(const std::string& username, const std::string& password);
    bool checkPermission(const std::string& username, const std::string& path, const std::string& action);

    // Audit logging
    void recordAudit(const std::string& user, const std::string& action, const std::string& target_path,
                     bool success, const std::string& details, const std::string& client_ip);

    // Transfer limits
    static constexpr uint64_t MAX_FILE_SIZE = 100ULL * 1024 * 1024; // 100 MB max for academic demo
    static constexpr size_t CHUNK_SIZE = 64 * 1024; // 64 KB chunks

private:
    std::shared_ptr<MetadataManager> metadata_;
};

} // namespace dfs
