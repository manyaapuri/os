#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

namespace dfs {

enum class NodeStatus {
    ONLINE,
    DEGRADED,
    OFFLINE
};

inline std::string nodeStatusToString(NodeStatus s) {
    switch (s) {
        case NodeStatus::ONLINE: return "ONLINE";
        case NodeStatus::DEGRADED: return "DEGRADED";
        case NodeStatus::OFFLINE: return "OFFLINE";
        default: return "UNKNOWN";
    }
}

struct NodeInfo {
    int id = 0;
    std::string name;
    std::string host = "127.0.0.1";
    int port = 0;
    std::string storage_path;
    uint64_t total_space = 0;
    uint64_t used_space = 0;
    NodeStatus status = NodeStatus::OFFLINE;
    int process_id = 0;
    int64_t last_heartbeat = 0; // Epoch seconds
    bool auto_launched = false;
};

struct FileChunk {
    int chunk_index = 0;
    std::string chunk_id;       // Typically {file_id}_chk{index}
    uint64_t offset = 0;
    uint64_t size = 0;
    std::string checksum_sha256;
    std::vector<int> replica_node_ids; // IDs of storage nodes holding this chunk
};

struct FileMetadata {
    int id = 0;
    std::string path;           // e.g. "/docs/report.pdf"
    std::string filename;       // "report.pdf"
    uint64_t size = 0;
    std::string owner = "admin";
    int permissions = 0755;     // Unix-style octal or bitmask
    int version = 1;
    std::string checksum_sha256;
    int replication_factor = 2; // e.g., 2 or 3
    int64_t created_at = 0;
    int64_t updated_at = 0;
    std::vector<FileChunk> chunks;
    bool is_deleted = false;
};

struct FileVersion {
    int version_id = 0;
    int file_id = 0;
    std::string path;
    int version = 1;
    uint64_t size = 0;
    std::string checksum_sha256;
    int64_t created_at = 0;
    std::string created_by;
};

enum class LockType {
    SHARED_READ,
    EXCLUSIVE_WRITE
};

inline std::string lockTypeToString(LockType lt) {
    return (lt == LockType::SHARED_READ) ? "SHARED_READ" : "EXCLUSIVE_WRITE";
}

struct LockInfo {
    std::string resource_path;
    LockType type;
    std::string client_id;
    int64_t acquired_at;
    int64_t expires_at;
    int thread_id = 0;
};

struct WaitingLockRequest {
    std::string request_id;
    std::string resource_path;
    LockType type;
    std::string client_id;
    int64_t requested_at;
    int timeout_ms = 5000;
};

enum class SchedulingAlgorithm {
    FCFS,
    PRIORITY,
    ROUND_ROBIN
};

inline std::string schedulingAlgorithmToString(SchedulingAlgorithm a) {
    switch (a) {
        case SchedulingAlgorithm::FCFS: return "FCFS";
        case SchedulingAlgorithm::PRIORITY: return "PRIORITY";
        case SchedulingAlgorithm::ROUND_ROBIN: return "ROUND_ROBIN";
        default: return "UNKNOWN";
    }
}

enum class RequestPriority {
    LOW = 0,
    NORMAL = 1,
    HIGH = 2,
    CRITICAL = 3
};

struct ScheduledRequest {
    std::string id;
    std::string operation;      // "READ", "WRITE", "DELETE", "REPLICATE"
    std::string path;
    std::string client_id;
    RequestPriority priority = RequestPriority::NORMAL;
    int time_quantum_ms = 50;   // For Round Robin
    int remaining_quantum_ms = 50;
    int64_t enqueue_time_ms = 0;
    int64_t start_time_ms = 0;
    int64_t finish_time_ms = 0;
    uint64_t bytes_transferred = 0;
};

struct SchedulerStats {
    SchedulingAlgorithm current_algorithm = SchedulingAlgorithm::FCFS;
    uint64_t total_requests_serviced = 0;
    double avg_waiting_time_ms = 0.0;
    double avg_response_time_ms = 0.0;
    double avg_turnaround_time_ms = 0.0;
    double throughput_ops_sec = 0.0;
    size_t queue_depth = 0;
};

struct BufferStats {
    size_t capacity = 0;
    size_t current_occupancy = 0;
    uint64_t total_produced = 0;
    uint64_t total_consumed = 0;
    uint64_t blocked_producers = 0;
    uint64_t blocked_consumers = 0;
    double high_watermark_pct = 0.0;
};

struct CacheStats {
    size_t capacity_items = 0;
    size_t current_items = 0;
    uint64_t hit_count = 0;
    uint64_t miss_count = 0;
    uint64_t eviction_count = 0;
    uint64_t invalidation_count = 0;
    double hit_ratio_pct = 0.0;
};

struct AuditLogEntry {
    int id = 0;
    int64_t timestamp = 0;
    std::string user;
    std::string action;        // "UPLOAD", "DOWNLOAD", "DELETE", "LOCK", "FAILOVER"
    std::string target_path;
    bool success = true;
    std::string details;
    std::string client_ip;
};

struct WalLogEntry {
    int64_t lsn = 0;            // Log Sequence Number
    int64_t timestamp = 0;
    std::string tx_id;
    std::string operation;      // "BEGIN", "PREPARE", "COMMIT", "ABORT", "WRITE_CHUNK", "METADATA_UPDATE"
    std::string file_path;
    std::string state;          // "PENDING", "COMMITTED", "ROLLED_BACK"
    std::string payload;
};

struct DiskSimRequest {
    int cylinder = 0;
    int sequence_id = 0;
};

struct DiskSimResult {
    std::string algorithm;
    std::vector<int> seek_sequence;
    int total_head_movement = 0;
    double avg_seek_distance = 0.0;
    int start_cylinder = 0;
};

} // namespace dfs
