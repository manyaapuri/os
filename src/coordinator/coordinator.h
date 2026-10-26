#pragma once

#include "common/types.h"
#include "metadata/metadata_manager.h"
#include "networking/tcp_server.h"
#include "networking/tcp_client.h"
#include "scheduler/request_scheduler.h"
#include "buffer/bounded_buffer.h"
#include "sync/lock_manager.h"
#include "cache/lru_cache.h"
#include "recovery/wal_manager.h"
#include "process/process_manager.h"
#include "security/security_manager.h"
#include "web/http_server.h"

#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

namespace dfs {

class Coordinator {
public:
    struct Config {
        int rpc_port = 9001;
        int web_port = 8080;
        std::string db_path = "data/dfs_metadata.db";
        std::string static_dir = "dashboard";
        int default_replication_factor = 2;
        int heartbeat_timeout_sec = 5;
        std::string storage_node_exe = "dfs_storage_node.exe";
    };

    Coordinator();
    explicit Coordinator(const Config& config);
    ~Coordinator();

    bool start();
    void stop();
    bool isRunning() const { return running_; }

    // Distributed File Operations (used by REST API, RPC, and test suite)
    bool uploadFile(const std::string& path, const std::vector<uint8_t>& data,
                    const std::string& owner = "admin", int replication_factor = 2,
                    RequestPriority priority = RequestPriority::NORMAL, int permissions = 0755);

    bool downloadFile(const std::string& path, std::vector<uint8_t>& out_data,
                      const std::string& user = "admin",
                      RequestPriority priority = RequestPriority::NORMAL);

    bool deleteFile(const std::string& path, const std::string& user = "admin");
    bool renameFile(const std::string& old_path, const std::string& new_path, const std::string& user = "admin");
    std::vector<FileMetadata> listFiles(const std::string& prefix = "");
    std::vector<FileMetadata> searchFiles(const std::string& query);
    std::optional<FileMetadata> getFileMetadata(const std::string& path);

    // Node & Process Management
    bool launchNodeProcess(int node_id);
    bool killNodeProcess(int node_id);
    void checkNodeHealth();
    void triggerReReplication();

    // Node Selection Strategy (Storage Allocation)
    std::vector<int> selectStorageNodesForChunks(int count, uint64_t chunk_size);

    // Getters for subsystems
    std::shared_ptr<MetadataManager> getMetadata() { return metadata_; }
    std::shared_ptr<RequestScheduler> getScheduler() { return scheduler_; }
    std::shared_ptr<LockManager> getLockManager() { return lock_manager_; }
    std::shared_ptr<LruFileCache> getCache() { return cache_; }
    std::shared_ptr<WalManager> getWalManager() { return wal_manager_; }
    std::shared_ptr<ProcessManager> getProcessManager() { return process_manager_; }
    std::shared_ptr<ChunkBuffer> getBuffer() { return chunk_buffer_; }

private:
    void setupRpcHandlers();
    void setupWebRoutes();
    void workerSchedulingLoop();
    void healthMonitorLoop();

    Config config_;
    std::atomic<bool> running_{false};

    std::shared_ptr<MetadataManager> metadata_;
    std::shared_ptr<RequestScheduler> scheduler_;
    std::shared_ptr<LockManager> lock_manager_;
    std::shared_ptr<LruFileCache> cache_;
    std::shared_ptr<WalManager> wal_manager_;
    std::shared_ptr<ProcessManager> process_manager_;
    std::shared_ptr<SecurityManager> security_manager_;
    std::shared_ptr<ChunkBuffer> chunk_buffer_;

    std::unique_ptr<TcpServer> rpc_server_;
    std::unique_ptr<HttpServer> http_server_;

    std::thread worker_thread_;
    std::thread health_thread_;
};

} // namespace dfs
