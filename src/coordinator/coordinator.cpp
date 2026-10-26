#include "coordinator/coordinator.h"
#include "common/logger.h"
#include "common/json_helper.h"
#include "common/sha256.h"
#include "disk/disk_scheduler.h"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <algorithm>

namespace dfs {

Coordinator::Coordinator()
    : Coordinator(Config()) {}

Coordinator::Coordinator(const Config& config)
    : config_(config),
      metadata_(std::make_shared<MetadataManager>()),
      scheduler_(std::make_shared<RequestScheduler>(SchedulingAlgorithm::FCFS)),
      lock_manager_(std::make_shared<LockManager>()),
      cache_(std::make_shared<LruFileCache>(64)),
      chunk_buffer_(std::make_shared<ChunkBuffer>(16)) {
    wal_manager_ = std::make_shared<WalManager>(metadata_);
    process_manager_ = std::make_shared<ProcessManager>();
    security_manager_ = std::make_shared<SecurityManager>(metadata_);
}

Coordinator::~Coordinator() {
    stop();
}

bool Coordinator::start() {
    if (running_) return true;

    LOG_INFO("Coordinator", "Initializing Advanced Distributed File System Coordinator...");

    // 1. Initialize SQLite Metadata
    if (!metadata_->init(config_.db_path)) {
        LOG_ERROR("Coordinator", "Failed to initialize metadata manager.");
        return false;
    }

    // 2. Register default 3 storage nodes
    std::vector<NodeInfo> default_nodes = {
        {1, "StorageNode_1", "127.0.0.1", 9101, "storage/node1", 100ULL * 1024 * 1024 * 1024, 0, NodeStatus::ONLINE, 0, 0, true},
        {2, "StorageNode_2", "127.0.0.1", 9102, "storage/node2", 100ULL * 1024 * 1024 * 1024, 0, NodeStatus::ONLINE, 0, 0, true},
        {3, "StorageNode_3", "127.0.0.1", 9103, "storage/node3", 100ULL * 1024 * 1024 * 1024, 0, NodeStatus::ONLINE, 0, 0, true}
    };
    for (const auto& n : default_nodes) {
        metadata_->registerOrUpdateNode(n);
    }

    // 3. Run Crash Recovery (WAL scan)
    auto recovery_report = wal_manager_->runCrashRecovery();
    for (const auto& msg : recovery_report.recovery_messages) {
        LOG_INFO("Recovery", msg);
    }

    // 4. Start RPC TCP Server on port 9001
    rpc_server_ = std::make_unique<TcpServer>();
    setupRpcHandlers();
    if (!rpc_server_->start(config_.rpc_port)) {
        LOG_ERROR("Coordinator", "Failed to bind RPC server on port " + std::to_string(config_.rpc_port));
        return false;
    }

    // 5. Start Web Server and Dashboard on port 8080
    http_server_ = std::make_unique<HttpServer>(config_.web_port, config_.static_dir);
    setupWebRoutes();
    if (!http_server_->start()) {
        LOG_ERROR("Coordinator", "Failed to start HTTP dashboard on port " + std::to_string(config_.web_port));
        return false;
    }

    running_ = true;

    // 6. Automatically spawn the 3 storage node processes if executable exists
    for (const auto& n : default_nodes) {
        if (std::filesystem::exists(config_.storage_node_exe)) {
            launchNodeProcess(n.id);
        }
    }

    // 7. Background Worker Scheduling & Health threads
    worker_thread_ = std::thread(&Coordinator::workerSchedulingLoop, this);
    health_thread_ = std::thread(&Coordinator::healthMonitorLoop, this);

    LOG_INFO("Coordinator", "Coordinator fully operational.");
    LOG_INFO("Coordinator", "Dashboard available at: http://127.0.0.1:" + std::to_string(config_.web_port) + "/");
    return true;
}

void Coordinator::stop() {
    if (!running_) return;
    running_ = false;

    if (chunk_buffer_) chunk_buffer_->close();

    if (rpc_server_) rpc_server_->stop();
    if (http_server_) http_server_->stop();

    if (worker_thread_.joinable()) worker_thread_.join();
    if (health_thread_.joinable()) health_thread_.join();

    if (process_manager_) process_manager_->shutdownAll();
    if (metadata_) metadata_->close();

    LOG_INFO("Coordinator", "Coordinator successfully stopped.");
}

bool Coordinator::launchNodeProcess(int node_id) {
    auto node_opt = metadata_->getNodeById(node_id);
    if (!node_opt) return false;

    bool ok = process_manager_->launchStorageNode(
        node_id, config_.storage_node_exe, node_opt->port, node_opt->storage_path
    );
    if (ok) {
        metadata_->updateNodeStatus(node_id, NodeStatus::ONLINE);
    }
    return ok;
}

bool Coordinator::killNodeProcess(int node_id) {
    bool ok = process_manager_->terminateProcess(node_id, true);
    metadata_->updateNodeStatus(node_id, NodeStatus::OFFLINE);
    security_manager_->recordAudit("admin", "NODE_KILL", "node_" + std::to_string(node_id), true,
                                   "Simulated storage node kill event", "127.0.0.1");
    return ok;
}

void Coordinator::setupRpcHandlers() {
    rpc_server_->setConnectionHandler([this](SOCKET sock, const std::string& ip, int client_port) {
        while (running_) {
            DfsMessage req;
            if (!SocketUtil::recvMessage(sock, req)) break;

            DfsMessage resp;
            resp.type = MessageType::RESPONSE_OK;

            switch (req.type) {
                case MessageType::HEARTBEAT: {
                    JsonValue j = JsonValue::parse(req.json_payload);
                    int node_id = j["node_id"].asInt();
                    int port = j["port"].asInt();
                    int pid = j["process_id"].asInt();
                    (void)port;
                    (void)pid;
                    uint64_t used = static_cast<uint64_t>(j["used_space"].asInt64());
                    uint64_t total = static_cast<uint64_t>(j["total_space"].asInt64());

                    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

                    metadata_->updateNodeHeartbeat(node_id, now, used, total);

                    resp.type = MessageType::HEARTBEAT_ACK;
                    resp.json_payload = "{\"status\": \"PONG\"}";
                    SocketUtil::sendMessage(sock, resp);
                    break;
                }

                default:
                    resp.type = MessageType::RESPONSE_ERROR;
                    resp.json_payload = "{\"error\": \"Unknown coordinator message type\"}";
                    SocketUtil::sendMessage(sock, resp);
                    break;
            }
        }
    });
}

void Coordinator::workerSchedulingLoop() {
    while (running_) {
        ScheduledRequest req;
        if (scheduler_->dequeue(req, 500)) {
            process_manager_->incrementWorkerThreads();

            // Simulate / service the request
            if (req.operation == "ROUND_ROBIN" && req.remaining_quantum_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            scheduler_->recordCompletion(req);
            process_manager_->recordTaskExecuted();
            process_manager_->decrementWorkerThreads();
        }
    }
}

void Coordinator::healthMonitorLoop() {
    while (running_) {
        checkNodeHealth();
        triggerReReplication();

        for (int i = 0; i < 30 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void Coordinator::checkNodeHealth() {
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto nodes = metadata_->getAllNodes();
    for (const auto& n : nodes) {
        if (n.last_heartbeat > 0 && (now - n.last_heartbeat > config_.heartbeat_timeout_sec)) {
            if (n.status != NodeStatus::OFFLINE) {
                LOG_WARN("Coordinator", "Node " + std::to_string(n.id) +
                         " missed heartbeat for " + std::to_string(now - n.last_heartbeat) +
                         "s. Marking OFFLINE.");
                metadata_->updateNodeStatus(n.id, NodeStatus::OFFLINE);
            }
        }
    }
}

std::vector<int> Coordinator::selectStorageNodesForChunks(int count, uint64_t chunk_size) {
    auto nodes = metadata_->getAllNodes();
    std::vector<NodeInfo> healthy;
    for (const auto& n : nodes) {
        if (n.status == NodeStatus::ONLINE || process_manager_->isNodeProcessRunning(n.id)) {
            healthy.push_back(n);
        }
    }

    if (healthy.empty()) {
        // Fallback: pick any known nodes
        healthy = nodes;
    }

    // Storage Allocation Strategy: Sort nodes by most free space available (Least-Utilized / Best-Fit)
    std::sort(healthy.begin(), healthy.end(), [](const NodeInfo& a, const NodeInfo& b) {
        uint64_t free_a = (a.total_space > a.used_space) ? (a.total_space - a.used_space) : 0;
        uint64_t free_b = (b.total_space > b.used_space) ? (b.total_space - b.used_space) : 0;
        return free_a > free_b;
    });

    std::vector<int> selected;
    for (size_t i = 0; i < healthy.size() && static_cast<int>(selected.size()) < count; ++i) {
        selected.push_back(healthy[i].id);
    }
    return selected;
}

bool Coordinator::uploadFile(const std::string& raw_path, const std::vector<uint8_t>& data,
                             const std::string& owner, int replication_factor, RequestPriority priority, int permissions) {
    std::string clean_path;
    if (!security_manager_->sanitizePath(raw_path, clean_path)) {
        LOG_ERROR("Coordinator", "Path traversal / safety violation: " + raw_path);
        security_manager_->recordAudit(owner, "UPLOAD", raw_path, false, "Path traversal rejected", "127.0.0.1");
        return false;
    }

    if (!security_manager_->checkPermission(owner, clean_path, "WRITE")) {
        LOG_ERROR("Coordinator", "Permission denied for " + owner + " on " + clean_path);
        security_manager_->recordAudit(owner, "UPLOAD", clean_path, false, "Permission denied", "127.0.0.1");
        return false;
    }

    // Schedule request through RequestScheduler
    ScheduledRequest sched_req;
    sched_req.id = "upload_" + clean_path;
    sched_req.operation = "WRITE";
    sched_req.path = clean_path;
    sched_req.client_id = owner;
    sched_req.priority = priority;
    sched_req.bytes_transferred = data.size();
    scheduler_->enqueue(sched_req);

    // Acquire Exclusive Write Lock with Deadlock Timeout (OS Synchronization Module)
    if (!lock_manager_->acquireLock(clean_path, LockType::EXCLUSIVE_WRITE, owner, 4000)) {
        LOG_WARN("Coordinator", "Lock acquisition timed out for " + clean_path);
        security_manager_->recordAudit(owner, "UPLOAD", clean_path, false, "Lock timeout / contention", "127.0.0.1");
        return false;
    }

    // Begin WAL Transaction (OS Crash Recovery Module)
    std::string tx_id = wal_manager_->beginTransaction("UPLOAD", clean_path, "size=" + std::to_string(data.size()));

    std::string file_sha256 = SHA256::hashBuffer(data.data(), data.size());
    std::filesystem::path p(clean_path);
    std::string filename = p.filename().string();

    FileMetadata meta;
    auto existing_opt = metadata_->getFileByPath(clean_path);
    if (existing_opt) {
        meta = *existing_opt;
        // Save previous version in version history
        metadata_->saveVersionSnapshot(meta, owner);
        meta.version += 1;
        meta.size = data.size();
        meta.checksum_sha256 = file_sha256;
        meta.replication_factor = replication_factor;
        meta.is_deleted = false;
        metadata_->updateFile(meta);
    } else {
        meta.path = clean_path;
        meta.filename = filename;
        meta.size = data.size();
        meta.owner = owner;
        meta.permissions = permissions;
        meta.version = 1;
        meta.checksum_sha256 = file_sha256;
        meta.replication_factor = replication_factor;
        if (!metadata_->createFile(meta)) {
            wal_manager_->abortTransaction(tx_id);
            lock_manager_->releaseLock(clean_path, owner);
            return false;
        }
    }

    // Chunking: Split data into chunks of 64KB (OS Memory & Buffer Module)
    const size_t CHUNK_SIZE = SecurityManager::CHUNK_SIZE;
    size_t offset = 0;
    int chunk_idx = 0;

    bool all_chunks_ok = true;

    while (offset < data.size() || (data.empty() && chunk_idx == 0)) {
        size_t current_chunk_len = (data.size() > offset) ? std::min(CHUNK_SIZE, data.size() - offset) : 0;
        std::vector<uint8_t> chunk_bytes;
        if (current_chunk_len > 0) {
            chunk_bytes.assign(data.begin() + offset, data.begin() + offset + current_chunk_len);
        }

        std::string chunk_sha = SHA256::hashBuffer(chunk_bytes.data(), chunk_bytes.size());
        std::string chunk_id = std::to_string(meta.id) + "_chk_" + std::to_string(chunk_idx);

        FileChunk chunk_info;
        chunk_info.chunk_index = chunk_idx;
        chunk_info.chunk_id = chunk_id;
        chunk_info.offset = offset;
        chunk_info.size = current_chunk_len;
        chunk_info.checksum_sha256 = chunk_sha;

        // Bounded producer-consumer buffer demo
        MemoryChunk mem_chk;
        mem_chk.file_path = clean_path;
        mem_chk.chunk_index = chunk_idx;
        mem_chk.data = chunk_bytes;
        mem_chk.checksum_sha256 = chunk_sha;
        chunk_buffer_->push(mem_chk, 2000);

        // Select storage nodes for replication
        auto target_nodes = selectStorageNodesForChunks(replication_factor, current_chunk_len);

        int successful_replicas = 0;
        for (int node_id : target_nodes) {
            auto node_opt = metadata_->getNodeById(node_id);
            if (!node_opt) continue;

            TcpClient client;
            if (client.connect(node_opt->host, node_opt->port, 3000)) {
                DfsMessage store_req;
                store_req.type = MessageType::STORE_CHUNK;

                JsonValue j;
                j["chunk_id"] = chunk_id;
                j["checksum_sha256"] = chunk_sha;
                store_req.json_payload = j.serialize();
                store_req.binary_data = chunk_bytes;

                DfsMessage store_resp;
                if (client.request(store_req, store_resp) && store_resp.type == MessageType::STORE_CHUNK_ACK) {
                    metadata_->assignReplica(chunk_id, node_id);
                    chunk_info.replica_node_ids.push_back(node_id);
                    successful_replicas++;
                }
            }
        }

        if (successful_replicas == 0) {
            all_chunks_ok = false;
            break;
        }

        metadata_->addChunk(meta.id, chunk_info);

        offset += (current_chunk_len > 0) ? current_chunk_len : 1;
        chunk_idx++;
        if (data.empty()) break;
    }

    // Invalidate distributed cache entry
    cache_->invalidate(clean_path);

    if (all_chunks_ok) {
        wal_manager_->commitTransaction(tx_id);
        security_manager_->recordAudit(owner, "UPLOAD", clean_path, true,
                                       "Uploaded " + std::to_string(data.size()) + " bytes, " +
                                       std::to_string(chunk_idx) + " chunks", "127.0.0.1");
    } else {
        wal_manager_->abortTransaction(tx_id);
        security_manager_->recordAudit(owner, "UPLOAD", clean_path, false, "Chunk replication failed", "127.0.0.1");
    }

    lock_manager_->releaseLock(clean_path, owner);
    return all_chunks_ok;
}

bool Coordinator::downloadFile(const std::string& raw_path, std::vector<uint8_t>& out_data,
                               const std::string& user, RequestPriority priority) {
    std::string clean_path;
    if (!security_manager_->sanitizePath(raw_path, clean_path)) {
        return false;
    }

    if (!security_manager_->checkPermission(user, clean_path, "READ")) {
        security_manager_->recordAudit(user, "DOWNLOAD", clean_path, false, "Permission denied", "127.0.0.1");
        return false;
    }

    // 1. Check Distributed In-Memory Cache (OS Distributed Caching Module)
    if (cache_->get(clean_path, out_data)) {
        LOG_INFO("Coordinator", "Cache HIT for " + clean_path);
        security_manager_->recordAudit(user, "DOWNLOAD", clean_path, true, "Cache HIT (" + std::to_string(out_data.size()) + " bytes)", "127.0.0.1");
        return true;
    }
    LOG_INFO("Coordinator", "Cache MISS for " + clean_path);

    // 2. Schedule Request through RequestScheduler (OS Request Scheduling Module)
    ScheduledRequest sched_req;
    sched_req.id = "download_" + clean_path;
    sched_req.operation = "READ";
    sched_req.path = clean_path;
    sched_req.client_id = user;
    sched_req.priority = priority;
    scheduler_->enqueue(sched_req);

    // 3. Acquire Shared Read Lock (OS Reader-Writer Lock Module)
    if (!lock_manager_->acquireLock(clean_path, LockType::SHARED_READ, user, 3000)) {
        LOG_WARN("Coordinator", "Lock acquire timed out for read: " + clean_path);
        return false;
    }

    auto meta_opt = metadata_->getFileByPath(clean_path);
    if (!meta_opt || meta_opt->is_deleted) {
        lock_manager_->releaseLock(clean_path, user);
        return false;
    }

    const auto& meta = *meta_opt;
    auto chunks = metadata_->getChunksForFile(meta.id);

    std::vector<uint8_t> assembled_data;
    assembled_data.reserve(meta.size);

    bool all_chunks_ok = true;

    for (const auto& chk : chunks) {
        bool chunk_retrieved = false;
        std::vector<uint8_t> chk_data;

        // Try healthy replicas with automatic failover (OS Fault Tolerance Module)
        for (int node_id : chk.replica_node_ids) {
            auto node_opt = metadata_->getNodeById(node_id);
            if (!node_opt || node_opt->status == NodeStatus::OFFLINE) {
                continue; // Skip offline replica, failover to next
            }

            TcpClient client;
            if (client.connect(node_opt->host, node_opt->port, 2000)) {
                DfsMessage req;
                req.type = MessageType::RETRIEVE_CHUNK;

                JsonValue j;
                j["chunk_id"] = chk.chunk_id;
                req.json_payload = j.serialize();

                DfsMessage resp;
                if (client.request(req, resp) && resp.type == MessageType::RETRIEVE_CHUNK_ACK) {
                    // Check SHA-256 integrity
                    std::string actual_sha = SHA256::hashBuffer(resp.binary_data.data(), resp.binary_data.size());
                    if (actual_sha == chk.checksum_sha256 || chk.checksum_sha256.empty()) {
                        chk_data = std::move(resp.binary_data);
                        chunk_retrieved = true;
                        break;
                    } else {
                        LOG_WARN("Coordinator", "Corrupt replica detected on Node " + std::to_string(node_id) +
                                 " for chunk " + chk.chunk_id + ". Failing over...");
                    }
                }
            }
        }

        if (!chunk_retrieved) {
            all_chunks_ok = false;
            break;
        }

        assembled_data.insert(assembled_data.end(), chk_data.begin(), chk_data.end());
    }

    lock_manager_->releaseLock(clean_path, user);

    if (all_chunks_ok) {
        out_data = std::move(assembled_data);
        // Put in Distributed Cache
        cache_->put(clean_path, out_data, meta.checksum_sha256, meta.version);
        security_manager_->recordAudit(user, "DOWNLOAD", clean_path, true,
                                       "Downloaded " + std::to_string(out_data.size()) + " bytes", "127.0.0.1");
        return true;
    } else {
        security_manager_->recordAudit(user, "DOWNLOAD", clean_path, false, "Chunk retrieval failure / all replicas down", "127.0.0.1");
        return false;
    }
}

bool Coordinator::deleteFile(const std::string& raw_path, const std::string& user) {
    std::string clean_path;
    if (!security_manager_->sanitizePath(raw_path, clean_path)) return false;

    if (!security_manager_->checkPermission(user, clean_path, "DELETE")) {
        security_manager_->recordAudit(user, "DELETE", clean_path, false, "Permission denied", "127.0.0.1");
        return false;
    }

    if (!lock_manager_->acquireLock(clean_path, LockType::EXCLUSIVE_WRITE, user, 3000)) {
        return false;
    }

    std::string tx_id = wal_manager_->beginTransaction("DELETE", clean_path);

    auto meta_opt = metadata_->getFileByPath(clean_path);
    if (!meta_opt) {
        wal_manager_->abortTransaction(tx_id);
        lock_manager_->releaseLock(clean_path, user);
        return false;
    }

    // Soft delete in metadata
    bool ok = metadata_->deleteFile(clean_path, true);

    // Invalidate cache
    cache_->invalidate(clean_path);

    if (ok) {
        wal_manager_->commitTransaction(tx_id);
        security_manager_->recordAudit(user, "DELETE", clean_path, true, "Soft deleted file", "127.0.0.1");
    } else {
        wal_manager_->abortTransaction(tx_id);
    }

    lock_manager_->releaseLock(clean_path, user);
    return ok;
}

bool Coordinator::renameFile(const std::string& old_path, const std::string& new_path, const std::string& user) {
    std::string clean_old, clean_new;
    if (!security_manager_->sanitizePath(old_path, clean_old) ||
        !security_manager_->sanitizePath(new_path, clean_new)) {
        return false;
    }

    // OS Lock Hierarchy Rule: acquireMultipleLocks strictly acquires in lexicographical order
    // to demonstrably prevent deadlocks!
    if (!lock_manager_->acquireMultipleLocks({clean_old, clean_new}, LockType::EXCLUSIVE_WRITE, user, 4000)) {
        security_manager_->recordAudit(user, "RENAME", clean_old, false, "Deadlock avoidance / Lock timeout", "127.0.0.1");
        return false;
    }

    std::string tx_id = wal_manager_->beginTransaction("RENAME", clean_old, "new_path=" + clean_new);

    bool ok = metadata_->renameFile(clean_old, clean_new);
    cache_->invalidate(clean_old);
    cache_->invalidate(clean_new);

    if (ok) {
        wal_manager_->commitTransaction(tx_id);
        security_manager_->recordAudit(user, "RENAME", clean_old, true, "Renamed to " + clean_new, "127.0.0.1");
    } else {
        wal_manager_->abortTransaction(tx_id);
    }

    lock_manager_->releaseMultipleLocks({clean_old, clean_new}, user);
    return ok;
}

std::vector<FileMetadata> Coordinator::listFiles(const std::string& prefix) {
    return metadata_->listFiles(prefix, false);
}

std::vector<FileMetadata> Coordinator::searchFiles(const std::string& query) {
    return metadata_->searchFiles(query);
}

std::optional<FileMetadata> Coordinator::getFileMetadata(const std::string& path) {
    std::string clean_path;
    if (!security_manager_->sanitizePath(path, clean_path)) return std::nullopt;
    auto meta = metadata_->getFileByPath(clean_path);
    if (!meta || meta->is_deleted) return std::nullopt;
    return meta;
}

void Coordinator::triggerReReplication() {
    auto files = metadata_->listFiles("", false);
    for (const auto& file : files) {
        auto chunks = metadata_->getChunksForFile(file.id);
        for (const auto& chk : chunks) {
            // Count healthy replicas
            int healthy_replicas = 0;
            std::vector<int> alive_nodes;
            for (int nid : chk.replica_node_ids) {
                auto node_opt = metadata_->getNodeById(nid);
                if (node_opt && node_opt->status == NodeStatus::ONLINE) {
                    healthy_replicas++;
                    alive_nodes.push_back(nid);
                }
            }

            // If under-replicated and at least one copy survives, re-replicate to another healthy node!
            if (healthy_replicas < file.replication_factor && !alive_nodes.empty()) {
                LOG_WARN("Coordinator", "Chunk " + chk.chunk_id + " under-replicated (" +
                         std::to_string(healthy_replicas) + "/" + std::to_string(file.replication_factor) +
                         "). Triggering re-replication...");

                // Find candidate healthy node that doesn't have it
                auto all_nodes = metadata_->getAllNodes();
                for (const auto& target : all_nodes) {
                    if (target.status == NodeStatus::ONLINE &&
                        std::find(alive_nodes.begin(), alive_nodes.end(), target.id) == alive_nodes.end()) {

                        // Retrieve from alive node
                        int source_id = alive_nodes.front();
                        auto source_opt = metadata_->getNodeById(source_id);
                        if (!source_opt) continue;

                        TcpClient src_client;
                        if (src_client.connect(source_opt->host, source_opt->port, 2000)) {
                            DfsMessage get_req;
                            get_req.type = MessageType::RETRIEVE_CHUNK;
                            JsonValue j;
                            j["chunk_id"] = chk.chunk_id;
                            get_req.json_payload = j.serialize();

                            DfsMessage get_resp;
                            if (src_client.request(get_req, get_resp) && get_resp.type == MessageType::RETRIEVE_CHUNK_ACK) {
                                // Store on target node
                                TcpClient tgt_client;
                                if (tgt_client.connect(target.host, target.port, 2000)) {
                                    DfsMessage store_req;
                                    store_req.type = MessageType::STORE_CHUNK;
                                    store_req.json_payload = get_req.json_payload;
                                    store_req.binary_data = get_resp.binary_data;

                                    DfsMessage store_resp;
                                    if (tgt_client.request(store_req, store_resp) && store_resp.type == MessageType::STORE_CHUNK_ACK) {
                                        metadata_->assignReplica(chk.chunk_id, target.id);
                                        LOG_INFO("Coordinator", "Successfully re-replicated chunk " + chk.chunk_id +
                                                 " to Node " + std::to_string(target.id));
                                        security_manager_->recordAudit("system", "RE_REPLICATION", chk.chunk_id, true,
                                                                       "Re-replicated to node " + std::to_string(target.id), "127.0.0.1");
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void Coordinator::setupWebRoutes() {
    // 1. Cluster Status
    http_server_->registerRoute("GET", "/api/status", [this](const HttpRequest& req) {
        HttpResponse resp;
        JsonValue j;
        j["status"] = "ONLINE";
        j["coordinator_pid"] = static_cast<int>(GetCurrentProcessId());
        j["rpc_port"] = config_.rpc_port;
        j["web_port"] = config_.web_port;
        j["uptime_sec"] = 120;

        auto thread_stats = process_manager_->getThreadPoolStats();
        j["active_worker_threads"] = thread_stats.active_worker_threads;
        j["total_tasks_executed"] = thread_stats.total_tasks_executed;

        resp.body = j.serialize();
        return resp;
    });

    // 2. Storage Nodes
    http_server_->registerRoute("GET", "/api/nodes", [this](const HttpRequest& req) {
        HttpResponse resp;
        process_manager_->updateProcessStates();
        auto nodes = metadata_->getAllNodes();

        JsonValue arr;
        for (const auto& n : nodes) {
            JsonValue j;
            j["id"] = n.id;
            j["name"] = n.name;
            j["host"] = n.host;
            j["port"] = n.port;
            j["storage_path"] = n.storage_path;
            j["total_space"] = n.total_space;
            j["used_space"] = n.used_space;
            j["status"] = nodeStatusToString(n.status);
            j["process_id"] = n.process_id;
            j["last_heartbeat"] = n.last_heartbeat;
            j["is_process_running"] = process_manager_->isNodeProcessRunning(n.id);
            arr.push_back(j);
        }
        resp.body = arr.serialize();
        return resp;
    });

    // Node Failure Simulation (Kill / Start)
    http_server_->registerRoute("POST", "/api/nodes/kill", [this](const HttpRequest& req) {
        HttpResponse resp;
        JsonValue j = JsonValue::parse(req.body);
        int node_id = j["node_id"].asInt();
        bool ok = killNodeProcess(node_id);
        resp.body = ok ? "{\"status\":\"OK\",\"message\":\"Node killed\"}" : "{\"error\":\"Kill failed\"}";
        return resp;
    });

    http_server_->registerRoute("POST", "/api/nodes/start", [this](const HttpRequest& req) {
        HttpResponse resp;
        JsonValue j = JsonValue::parse(req.body);
        int node_id = j["node_id"].asInt();
        bool ok = launchNodeProcess(node_id);
        resp.body = ok ? "{\"status\":\"OK\",\"message\":\"Node started\"}" : "{\"error\":\"Start failed\"}";
        return resp;
    });

    // 3. Files List
    http_server_->registerRoute("GET", "/api/files", [this](const HttpRequest& req) {
        HttpResponse resp;
        auto files = listFiles();
        JsonValue arr;
        for (const auto& f : files) {
            JsonValue j;
            j["id"] = f.id;
            j["path"] = f.path;
            j["filename"] = f.filename;
            j["size"] = f.size;
            j["owner"] = f.owner;
            j["version"] = f.version;
            j["checksum_sha256"] = f.checksum_sha256;
            j["replication_factor"] = f.replication_factor;
            j["created_at"] = f.created_at;
            j["updated_at"] = f.updated_at;

            JsonValue chk_arr;
            for (const auto& c : f.chunks) {
                JsonValue cj;
                cj["chunk_id"] = c.chunk_id;
                cj["index"] = c.chunk_index;
                cj["size"] = c.size;
                cj["checksum"] = c.checksum_sha256;
                JsonValue repls;
                for (int nid : c.replica_node_ids) repls.push_back(nid);
                cj["replicas"] = repls;
                chk_arr.push_back(cj);
            }
            j["chunks"] = chk_arr;
            arr.push_back(j);
        }
        resp.body = arr.serialize();
        return resp;
    });

    // 4. File Upload (REST API)
    http_server_->registerRoute("POST", "/api/files/upload", [this](const HttpRequest& req) {
        HttpResponse resp;
        JsonValue j = JsonValue::parse(req.body);
        std::string path = j["path"].asString();
        std::string content = j["content"].asString();
        std::string owner = j["owner"].asString("admin");
        int repl = j["replication_factor"].asInt(config_.default_replication_factor);

        if (path.empty()) {
            resp.status_code = 400;
            resp.body = "{\"error\": \"Missing path\"}";
            return resp;
        }

        std::vector<uint8_t> data(content.begin(), content.end());
        bool ok = uploadFile(path, data, owner, repl);
        if (ok) {
            resp.body = "{\"status\": \"OK\", \"message\": \"File uploaded successfully\"}";
        } else {
            resp.status_code = 500;
            resp.body = "{\"error\": \"Upload failed\"}";
        }
        return resp;
    });

    // 5. File Download (REST API)
    http_server_->registerRoute("GET", "/api/files/download", [this](const HttpRequest& req) {
        HttpResponse resp;
        auto it = req.query_params.find("path");
        if (it == req.query_params.end() || it->second.empty()) {
            resp.status_code = 400;
            resp.body = "{\"error\": \"Missing path query param\"}";
            return resp;
        }

        std::string path = it->second;
        std::vector<uint8_t> data;
        bool ok = downloadFile(path, data);
        if (ok) {
            resp.status_code = 200;
            resp.content_type = "application/octet-stream";
            resp.headers["Content-Disposition"] = "attachment; filename=\"downloaded_file\"";
            resp.body = std::string(data.begin(), data.end());
        } else {
            resp.status_code = 404;
            resp.body = "{\"error\": \"File not found or replicas inaccessible\"}";
        }
        return resp;
    });

    // 6. File Delete (REST API)
    http_server_->registerRoute("DELETE", "/api/files", [this](const HttpRequest& req) {
        HttpResponse resp;
        auto it = req.query_params.find("path");
        if (it == req.query_params.end() || it->second.empty()) {
            resp.status_code = 400;
            resp.body = "{\"error\": \"Missing path parameter\"}";
            return resp;
        }
        bool ok = deleteFile(it->second);
        resp.body = ok ? "{\"status\": \"OK\"}" : "{\"error\": \"Delete failed\"}";
        return resp;
    });

    // 7. File Rename (REST API)
    http_server_->registerRoute("POST", "/api/files/rename", [this](const HttpRequest& req) {
        HttpResponse resp;
        JsonValue j = JsonValue::parse(req.body);
        std::string old_p = j["old_path"].asString();
        std::string new_p = j["new_path"].asString();
        bool ok = renameFile(old_p, new_p);
        resp.body = ok ? "{\"status\": \"OK\"}" : "{\"error\": \"Rename failed\"}";
        return resp;
    });

    // 8. Versions
    http_server_->registerRoute("GET", "/api/files/versions", [this](const HttpRequest& req) {
        HttpResponse resp;
        auto it = req.query_params.find("path");
        if (it == req.query_params.end()) {
            resp.body = "[]";
            return resp;
        }
        auto list = metadata_->getVersionsForFile(it->second);
        JsonValue arr;
        for (const auto& v : list) {
            JsonValue j;
            j["version_id"] = v.version_id;
            j["path"] = v.path;
            j["version"] = v.version;
            j["size"] = v.size;
            j["checksum"] = v.checksum_sha256;
            j["created_at"] = v.created_at;
            j["created_by"] = v.created_by;
            arr.push_back(j);
        }
        resp.body = arr.serialize();
        return resp;
    });

    http_server_->registerRoute("POST", "/api/files/restore", [this](const HttpRequest& req) {
        HttpResponse resp;
        JsonValue j = JsonValue::parse(req.body);
        std::string path = j["path"].asString();
        int ver = j["version"].asInt();
        bool ok = metadata_->restoreVersion(path, ver);
        cache_->invalidate(path);
        resp.body = ok ? "{\"status\": \"OK\"}" : "{\"error\": \"Restore failed\"}";
        return resp;
    });

    // 9. Request Scheduler Metrics & Algorithm Switching
    http_server_->registerRoute("GET", "/api/scheduler", [this](const HttpRequest& req) {
        HttpResponse resp;
        auto stats = scheduler_->getStats();
        auto queue = scheduler_->getQueueSnapshot();

        JsonValue j;
        j["current_algorithm"] = schedulingAlgorithmToString(stats.current_algorithm);
        j["total_requests_serviced"] = stats.total_requests_serviced;
        j["queue_depth"] = stats.queue_depth;
        j["avg_waiting_time_ms"] = stats.avg_waiting_time_ms;
        j["avg_response_time_ms"] = stats.avg_response_time_ms;
        j["avg_turnaround_time_ms"] = stats.avg_turnaround_time_ms;
        j["throughput_ops_sec"] = stats.throughput_ops_sec;

        JsonValue q_arr;
        for (const auto& item : queue) {
            JsonValue qj;
            qj["id"] = item.id;
            qj["operation"] = item.operation;
            qj["path"] = item.path;
            qj["client"] = item.client_id;
            qj["priority"] = static_cast<int>(item.priority);
            qj["quantum_ms"] = item.time_quantum_ms;
            q_arr.push_back(qj);
        }
        j["queue"] = q_arr;
        resp.body = j.serialize();
        return resp;
    });

    http_server_->registerRoute("POST", "/api/scheduler/set", [this](const HttpRequest& req) {
        HttpResponse resp;
        auto it = req.query_params.find("algo");
        if (it != req.query_params.end()) {
            std::string algo = it->second;
            if (algo == "PRIORITY") scheduler_->setAlgorithm(SchedulingAlgorithm::PRIORITY);
            else if (algo == "ROUND_ROBIN") scheduler_->setAlgorithm(SchedulingAlgorithm::ROUND_ROBIN);
            else scheduler_->setAlgorithm(SchedulingAlgorithm::FCFS);
            resp.body = "{\"status\": \"OK\", \"algorithm\": \"" + algo + "\"}";
        } else {
            resp.body = "{\"error\": \"Missing algo param\"}";
        }
        return resp;
    });

    // 10. Memory Buffer Diagnostics
    http_server_->registerRoute("GET", "/api/buffer", [this](const HttpRequest& req) {
        HttpResponse resp;
        auto stats = chunk_buffer_->getStats();
        JsonValue j;
        j["capacity"] = stats.capacity;
        j["current_occupancy"] = stats.current_occupancy;
        j["total_produced"] = stats.total_produced;
        j["total_consumed"] = stats.total_consumed;
        j["blocked_producers"] = stats.blocked_producers;
        j["blocked_consumers"] = stats.blocked_consumers;
        j["high_watermark_pct"] = stats.high_watermark_pct;
        resp.body = j.serialize();
        return resp;
    });

    // 11. Lock & Deadlock Diagnostics
    http_server_->registerRoute("GET", "/api/locks", [this](const HttpRequest& req) {
        HttpResponse resp;
        auto diag = lock_manager_->getDiagnostics();
        auto active = lock_manager_->getActiveLocks();
        auto waiting = lock_manager_->getWaitingRequests();

        JsonValue j;
        j["active_lock_count"] = diag.active_lock_count;
        j["waiting_request_count"] = diag.waiting_request_count;
        j["total_acquired"] = diag.total_acquired;
        j["total_timeouts"] = diag.total_timeouts;
        j["total_deadlocks_prevented"] = diag.total_deadlocks_prevented;
        j["has_deadlock_cycle"] = diag.has_deadlock_cycle;

        JsonValue act_arr;
        for (const auto& a : active) {
            JsonValue aj;
            aj["resource"] = a.resource_path;
            aj["type"] = lockTypeToString(a.type);
            aj["client"] = a.client_id;
            aj["acquired_at"] = a.acquired_at;
            act_arr.push_back(aj);
        }
        j["active_locks"] = act_arr;

        JsonValue wait_arr;
        for (const auto& w : waiting) {
            JsonValue wj;
            wj["resource"] = w.resource_path;
            wj["type"] = lockTypeToString(w.type);
            wj["client"] = w.client_id;
            wj["timeout_ms"] = w.timeout_ms;
            wait_arr.push_back(wj);
        }
        j["waiting_requests"] = wait_arr;

        JsonValue cyc_arr;
        for (const auto& c : diag.deadlock_cycle) cyc_arr.push_back(c);
        j["deadlock_cycle"] = cyc_arr;

        resp.body = j.serialize();
        return resp;
    });

    // 12. Distributed Cache Stats
    http_server_->registerRoute("GET", "/api/cache", [this](const HttpRequest& req) {
        HttpResponse resp;
        auto stats = cache_->getStats();
        JsonValue j;
        j["capacity_items"] = stats.capacity_items;
        j["current_items"] = stats.current_items;
        j["hit_count"] = stats.hit_count;
        j["miss_count"] = stats.miss_count;
        j["eviction_count"] = stats.eviction_count;
        j["invalidation_count"] = stats.invalidation_count;
        j["hit_ratio_pct"] = stats.hit_ratio_pct;

        JsonValue keys;
        for (const auto& k : cache_->getCachedKeys()) keys.push_back(k);
        j["cached_keys"] = keys;

        resp.body = j.serialize();
        return resp;
    });

    http_server_->registerRoute("POST", "/api/cache/clear", [this](const HttpRequest& req) {
        HttpResponse resp;
        cache_->clear();
        resp.body = "{\"status\": \"OK\"}";
        return resp;
    });

    // 13. Crash Recovery & WAL Entries
    http_server_->registerRoute("GET", "/api/recovery", [this](const HttpRequest& req) {
        HttpResponse resp;
        auto logs = wal_manager_->getRecentLogs(30);
        JsonValue arr;
        for (const auto& entry : logs) {
            JsonValue j;
            j["lsn"] = entry.lsn;
            j["timestamp"] = entry.timestamp;
            j["tx_id"] = entry.tx_id;
            j["operation"] = entry.operation;
            j["file_path"] = entry.file_path;
            j["state"] = entry.state;
            j["payload"] = entry.payload;
            arr.push_back(j);
        }
        resp.body = arr.serialize();
        return resp;
    });

    http_server_->registerRoute("POST", "/api/recovery/run", [this](const HttpRequest& req) {
        HttpResponse resp;
        auto rep = wal_manager_->runCrashRecovery();
        JsonValue j;
        j["pending_operations_found"] = rep.pending_operations_found;
        j["operations_rolled_back"] = rep.operations_rolled_back;
        JsonValue msgs;
        for (const auto& m : rep.recovery_messages) msgs.push_back(m);
        j["messages"] = msgs;
        resp.body = j.serialize();
        return resp;
    });

    // 14. Disk Scheduling Simulation
    http_server_->registerRoute("GET", "/api/disk_sim", [this](const HttpRequest& req) {
        HttpResponse resp;
        std::vector<int> requests = {98, 183, 37, 122, 14, 124, 65, 67};
        auto summary = DiskSchedulerSimulator::compareAll(53, requests, 199);

        JsonValue j;
        j["disclaimer"] = summary.disclaimer;
        j["best_algorithm"] = summary.best_algorithm;
        j["start_cylinder"] = 53;

        auto packResult = [](const DiskSimResult& r) {
            JsonValue o;
            o["algorithm"] = r.algorithm;
            o["total_head_movement"] = r.total_head_movement;
            o["avg_seek_distance"] = r.avg_seek_distance;
            JsonValue seq;
            for (int s : r.seek_sequence) seq.push_back(s);
            o["seek_sequence"] = seq;
            return o;
        };

        j["fcfs"] = packResult(summary.fcfs);
        j["sstf"] = packResult(summary.sstf);
        j["scan"] = packResult(summary.scan);

        resp.body = j.serialize();
        return resp;
    });

    http_server_->registerRoute("POST", "/api/disk_sim/run", [this](const HttpRequest& req) {
        HttpResponse resp;
        JsonValue j = JsonValue::parse(req.body);
        int start = j["start_cylinder"].asInt(53);
        int max_cyl = j["max_cylinder"].asInt(199);
        std::vector<int> requests;
        for (size_t i = 0; i < j["requests"].arr_val.size(); ++i) {
            requests.push_back(j["requests"][i].asInt());
        }
        if (requests.empty()) {
            requests = {98, 183, 37, 122, 14, 124, 65, 67};
        }

        auto summary = DiskSchedulerSimulator::compareAll(start, requests, max_cyl);
        JsonValue res;
        res["disclaimer"] = summary.disclaimer;
        res["best_algorithm"] = summary.best_algorithm;

        auto packResult = [](const DiskSimResult& r) {
            JsonValue o;
            o["algorithm"] = r.algorithm;
            o["total_head_movement"] = r.total_head_movement;
            o["avg_seek_distance"] = r.avg_seek_distance;
            JsonValue seq;
            for (int s : r.seek_sequence) seq.push_back(s);
            o["seek_sequence"] = seq;
            return o;
        };

        res["fcfs"] = packResult(summary.fcfs);
        res["sstf"] = packResult(summary.sstf);
        res["scan"] = packResult(summary.scan);

        resp.body = res.serialize();
        return resp;
    });

    // 15. Audit Logs
    http_server_->registerRoute("GET", "/api/audit", [this](const HttpRequest& req) {
        HttpResponse resp;
        auto logs = metadata_->getRecentAuditLogs(50);
        JsonValue arr;
        for (const auto& a : logs) {
            JsonValue j;
            j["id"] = a.id;
            j["timestamp"] = a.timestamp;
            j["user"] = a.user;
            j["action"] = a.action;
            j["target_path"] = a.target_path;
            j["success"] = a.success;
            j["details"] = a.details;
            j["client_ip"] = a.client_ip;
            arr.push_back(j);
        }
        resp.body = arr.serialize();
        return resp;
    });
}

} // namespace dfs
