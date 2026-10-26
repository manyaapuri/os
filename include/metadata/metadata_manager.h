#pragma once

#include "common/types.h"
#include <string>
#include <vector>
#include <optional>
#include <mutex>

struct sqlite3;

namespace dfs {

class MetadataManager {
public:
    MetadataManager();
    ~MetadataManager();

    bool init(const std::string& db_path = "data/dfs_metadata.db");
    void close();

    // File Metadata
    bool createFile(FileMetadata& file);
    std::optional<FileMetadata> getFileByPath(const std::string& path);
    std::optional<FileMetadata> getFileById(int id);
    bool updateFile(const FileMetadata& file);
    bool renameFile(const std::string& old_path, const std::string& new_path);
    bool deleteFile(const std::string& path, bool soft_delete = true);
    std::vector<FileMetadata> listFiles(const std::string& prefix = "", bool include_deleted = false);
    std::vector<FileMetadata> searchFiles(const std::string& query);

    // Chunks & Replicas
    bool addChunk(int file_id, const FileChunk& chunk);
    bool assignReplica(const std::string& chunk_id, int node_id);
    bool removeReplica(const std::string& chunk_id, int node_id);
    std::vector<FileChunk> getChunksForFile(int file_id);
    std::vector<int> getReplicasForChunk(const std::string& chunk_id);

    // Versioning
    bool saveVersionSnapshot(const FileMetadata& file, const std::string& created_by);
    std::vector<FileVersion> getVersionsForFile(const std::string& path);
    std::optional<FileVersion> getVersion(const std::string& path, int version);
    bool restoreVersion(const std::string& path, int target_version);

    // Storage Nodes
    bool registerOrUpdateNode(const NodeInfo& node);
    bool updateNodeHeartbeat(int node_id, int64_t heartbeat_epoch_sec, uint64_t used_space, uint64_t total_space);
    bool updateNodeStatus(int node_id, NodeStatus status);
    std::vector<NodeInfo> getAllNodes();
    std::optional<NodeInfo> getNodeById(int node_id);

    // Audit Log
    bool addAuditLog(const std::string& user, const std::string& action, const std::string& target_path,
                     bool success, const std::string& details, const std::string& client_ip = "127.0.0.1");
    std::vector<AuditLogEntry> getRecentAuditLogs(int limit = 100);

    // Write-Ahead Log (WAL)
    int64_t appendWal(const std::string& tx_id, const std::string& op, const std::string& file_path,
                      const std::string& state, const std::string& payload);
    bool updateWalState(const std::string& tx_id, const std::string& new_state);
    std::vector<WalLogEntry> getPendingWalEntries();
    std::vector<WalLogEntry> getRecentWalEntries(int limit = 50);

    // Authentication & Access Control
    bool initDefaultUsers();
    bool verifyUser(const std::string& username, const std::string& password);
    bool hasPermission(const std::string& username, const std::string& path, const std::string& op);

private:
    bool createTables();
    bool executeSql(const std::string& sql);

    sqlite3* db_ = nullptr;
    std::string db_path_;
    std::mutex db_mutex_;
};

} // namespace dfs
