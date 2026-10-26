#include "security/security_manager.h"
#include "common/logger.h"
#include <filesystem>
#include <sstream>

namespace dfs {

SecurityManager::SecurityManager(std::shared_ptr<MetadataManager> metadata_mgr)
    : metadata_(metadata_mgr) {}

SecurityManager::~SecurityManager() = default;

bool SecurityManager::isPathSafe(const std::string& path) {
    if (path.empty()) return false;

    // Check for null bytes or control characters
    for (char c : path) {
        if (static_cast<unsigned char>(c) < 32) return false;
    }

    // Check for traversal sequences
    if (path.find("..") != std::string::npos) return false;
    if (path.find("./") != std::string::npos || path.find(".\\") != std::string::npos) return false;
    if (path.find(':') != std::string::npos) return false; // Windows drive letters

    // Block sensitive OS and UNIX path patterns
    std::string lower = path;
    for (char& c : lower) c = static_cast<char>(tolower(c));
    if (lower.find("windows") != std::string::npos ||
        lower.find("system32") != std::string::npos ||
        lower.find("etc/passwd") != std::string::npos ||
        lower.find("etc/shadow") != std::string::npos ||
        lower.rfind("/etc", 0) == 0) {
        return false;
    }

    return true;
}

bool SecurityManager::sanitizePath(const std::string& raw_path, std::string& clean_path) {
    if (!isPathSafe(raw_path)) return false;

    std::string norm = raw_path;
    // Replace backslashes with forward slashes
    for (char& c : norm) {
        if (c == '\\') c = '/';
    }

    // Ensure leading slash
    if (norm.empty() || norm[0] != '/') {
        norm = "/" + norm;
    }

    // Collapse multiple consecutive slashes
    std::string collapsed;
    bool last_was_slash = false;
    for (char c : norm) {
        if (c == '/') {
            if (!last_was_slash) {
                collapsed += c;
                last_was_slash = true;
            }
        } else {
            collapsed += c;
            last_was_slash = false;
        }
    }

    // Disallow trailing slash unless root
    if (collapsed.size() > 1 && collapsed.back() == '/') {
        collapsed.pop_back();
    }

    clean_path = collapsed;
    return true;
}

bool SecurityManager::authenticate(const std::string& username, const std::string& password) {
    return metadata_->verifyUser(username, password);
}

bool SecurityManager::checkPermission(const std::string& username, const std::string& path, const std::string& action) {
    return metadata_->hasPermission(username, path, action);
}

void SecurityManager::recordAudit(const std::string& user, const std::string& action, const std::string& target_path,
                                  bool success, const std::string& details, const std::string& client_ip) {
    metadata_->addAuditLog(user, action, target_path, success, details, client_ip);
}

} // namespace dfs
