#include "storage_node/storage_node.h"
#include "common/logger.h"
#include "common/sha256.h"
#include "common/json_helper.h"
#include "recovery/wal_manager.h"
#include <filesystem>
#include <fstream>
#include <windows.h>

namespace dfs {

StorageNode::StorageNode(int node_id, int port, const std::string& storage_dir,
                         int coordinator_port, const std::string& coordinator_host)
    : node_id_(node_id), port_(port), storage_dir_(storage_dir),
      coordinator_port_(coordinator_port), coordinator_host_(coordinator_host),
      chunk_buffer_(16) {
    std::filesystem::create_directories(storage_dir_);
}

StorageNode::~StorageNode() {
    stop();
}

bool StorageNode::start() {
    if (running_) return true;

    server_ = std::make_unique<TcpServer>();
    server_->setConnectionHandler([this](SOCKET sock, const std::string& ip, int client_port) {
        handleClient(sock, ip, client_port);
    });

    if (!server_->start(port_)) {
        LOG_ERROR("StorageNode", "Failed to start server on port " + std::to_string(port_));
        return false;
    }

    running_ = true;
    heartbeat_thread_ = std::thread(&StorageNode::heartbeatLoop, this);

    LOG_INFO("StorageNode", "Storage Node " + std::to_string(node_id_) +
             " started on port " + std::to_string(port_) +
             " [Dir: " + storage_dir_ + ", PID: " + std::to_string(GetCurrentProcessId()) + "]");
    return true;
}

void StorageNode::stop() {
    if (!running_) return;
    running_ = false;

    if (server_) {
        server_->stop();
    }

    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    LOG_INFO("StorageNode", "Storage Node " + std::to_string(node_id_) + " stopped.");
}

uint64_t StorageNode::getUsedSpace() const {
    uint64_t total = 0;
    try {
        if (std::filesystem::exists(storage_dir_)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(storage_dir_)) {
                if (entry.is_regular_file()) {
                    total += entry.file_size();
                }
            }
        }
    } catch (...) {}
    return total;
}

uint64_t StorageNode::getTotalSpace() const {
    try {
        auto space = std::filesystem::space(storage_dir_);
        return space.capacity;
    } catch (...) {
        return 100ULL * 1024 * 1024 * 1024; // 100GB fallback
    }
}

void StorageNode::heartbeatLoop() {
    while (running_) {
        TcpClient client;
        if (client.connect(coordinator_host_, coordinator_port_, 2000)) {
            DfsMessage req;
            req.type = MessageType::HEARTBEAT;

            JsonValue j;
            j["node_id"] = node_id_;
            j["port"] = port_;
            j["process_id"] = static_cast<int>(GetCurrentProcessId());
            j["storage_path"] = storage_dir_;
            j["used_space"] = static_cast<int64_t>(getUsedSpace());
            j["total_space"] = static_cast<int64_t>(getTotalSpace());
            req.json_payload = j.serialize();

            DfsMessage resp;
            client.request(req, resp);
        }

        // Sleep for 2 seconds with quick cancellation check
        for (int i = 0; i < 20 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void StorageNode::handleClient(SOCKET sock, const std::string& ip, int client_port) {
    while (running_) {
        DfsMessage req;
        if (!SocketUtil::recvMessage(sock, req)) {
            break;
        }

        DfsMessage resp;
        resp.type = MessageType::RESPONSE_OK;

        switch (req.type) {
            case MessageType::STORE_CHUNK: {
                JsonValue j = JsonValue::parse(req.json_payload);
                std::string chunk_id = j["chunk_id"].asString();
                std::string expected_sha256 = j["checksum_sha256"].asString();

                if (chunk_id.empty() || req.binary_data.empty()) {
                    resp.type = MessageType::RESPONSE_ERROR;
                    resp.json_payload = "{\"error\": \"Empty chunk_id or data\"}";
                    SocketUtil::sendMessage(sock, resp);
                    break;
                }

                // Verify received checksum
                std::string actual_sha256 = SHA256::hashBuffer(req.binary_data.data(), req.binary_data.size());
                if (!expected_sha256.empty() && actual_sha256 != expected_sha256) {
                    LOG_ERROR("StorageNode", "Checksum mismatch for chunk: " + chunk_id);
                    resp.type = MessageType::RESPONSE_ERROR;
                    resp.json_payload = "{\"error\": \"SHA256 checksum mismatch\"}";
                    SocketUtil::sendMessage(sock, resp);
                    break;
                }

                // Producer-Consumer Buffer: push chunk through bounded buffer
                MemoryChunk mem_chk;
                mem_chk.file_path = chunk_id;
                mem_chk.data = req.binary_data;
                mem_chk.checksum_sha256 = actual_sha256;
                chunk_buffer_.push(mem_chk, 5000);

                // Consumer pops from buffer and writes atomically to disk
                MemoryChunk item;
                if (chunk_buffer_.pop(item, 5000)) {
                    std::string chunk_file_path = storage_dir_ + "/" + chunk_id + ".chk";
                    std::string out_sha;
                    bool ok = WalManager::atomicWriteFile(chunk_file_path, item.data.data(), item.data.size(), out_sha);

                    if (ok) {
                        resp.type = MessageType::STORE_CHUNK_ACK;
                        JsonValue res_j;
                        res_j["status"] = "OK";
                        res_j["chunk_id"] = chunk_id;
                        res_j["checksum_sha256"] = out_sha;
                        resp.json_payload = res_j.serialize();
                        LOG_INFO("StorageNode", "Stored chunk " + chunk_id + " (" + std::to_string(item.data.size()) + " bytes)");
                    } else {
                        resp.type = MessageType::RESPONSE_ERROR;
                        resp.json_payload = "{\"error\": \"Atomic disk write failed\"}";
                    }
                } else {
                    resp.type = MessageType::RESPONSE_ERROR;
                    resp.json_payload = "{\"error\": \"Bounded buffer timeout\"}";
                }

                SocketUtil::sendMessage(sock, resp);
                break;
            }

            case MessageType::RETRIEVE_CHUNK: {
                JsonValue j = JsonValue::parse(req.json_payload);
                std::string chunk_id = j["chunk_id"].asString();
                std::string chunk_file_path = storage_dir_ + "/" + chunk_id + ".chk";

                if (!std::filesystem::exists(chunk_file_path)) {
                    resp.type = MessageType::RESPONSE_ERROR;
                    resp.json_payload = "{\"error\": \"Chunk not found\"}";
                    SocketUtil::sendMessage(sock, resp);
                    break;
                }

                std::ifstream f(chunk_file_path, std::ios::binary | std::ios::ate);
                if (!f) {
                    resp.type = MessageType::RESPONSE_ERROR;
                    resp.json_payload = "{\"error\": \"Failed to read chunk file\"}";
                    SocketUtil::sendMessage(sock, resp);
                    break;
                }

                std::streamsize size = f.tellg();
                f.seekg(0, std::ios::beg);

                resp.binary_data.resize(static_cast<size_t>(size));
                if (f.read(reinterpret_cast<char*>(resp.binary_data.data()), size)) {
                    resp.type = MessageType::RETRIEVE_CHUNK_ACK;
                    std::string sha = SHA256::hashBuffer(resp.binary_data.data(), resp.binary_data.size());
                    JsonValue res_j;
                    res_j["status"] = "OK";
                    res_j["chunk_id"] = chunk_id;
                    res_j["size"] = static_cast<int64_t>(size);
                    res_j["checksum_sha256"] = sha;
                    resp.json_payload = res_j.serialize();
                } else {
                    resp.type = MessageType::RESPONSE_ERROR;
                    resp.json_payload = "{\"error\": \"Failed to read chunk bytes\"}";
                }
                SocketUtil::sendMessage(sock, resp);
                break;
            }

            case MessageType::DELETE_CHUNK: {
                JsonValue j = JsonValue::parse(req.json_payload);
                std::string chunk_id = j["chunk_id"].asString();
                std::string chunk_file_path = storage_dir_ + "/" + chunk_id + ".chk";

                if (std::filesystem::exists(chunk_file_path)) {
                    std::filesystem::remove(chunk_file_path);
                }
                resp.type = MessageType::DELETE_CHUNK_ACK;
                resp.json_payload = "{\"status\": \"OK\"}";
                SocketUtil::sendMessage(sock, resp);
                break;
            }

            case MessageType::VERIFY_CHUNK: {
                JsonValue j = JsonValue::parse(req.json_payload);
                std::string chunk_id = j["chunk_id"].asString();
                std::string expected_sha = j["expected_checksum"].asString();
                std::string chunk_file_path = storage_dir_ + "/" + chunk_id + ".chk";

                bool exists = std::filesystem::exists(chunk_file_path);
                bool valid = false;
                std::string actual_sha;

                if (exists) {
                    actual_sha = SHA256::hashFile(chunk_file_path);
                    valid = (actual_sha == expected_sha);
                }

                resp.type = MessageType::VERIFY_CHUNK_ACK;
                JsonValue res_j;
                res_j["exists"] = exists;
                res_j["valid"] = valid;
                res_j["actual_checksum"] = actual_sha;
                resp.json_payload = res_j.serialize();
                SocketUtil::sendMessage(sock, resp);
                break;
            }

            case MessageType::GET_NODE_STATS: {
                resp.type = MessageType::GET_NODE_STATS_ACK;
                JsonValue res_j;
                res_j["node_id"] = node_id_;
                res_j["port"] = port_;
                res_j["used_space"] = static_cast<int64_t>(getUsedSpace());
                res_j["total_space"] = static_cast<int64_t>(getTotalSpace());
                res_j["process_id"] = static_cast<int>(GetCurrentProcessId());
                resp.json_payload = res_j.serialize();
                SocketUtil::sendMessage(sock, resp);
                break;
            }

            case MessageType::SHUTDOWN_NODE: {
                resp.type = MessageType::RESPONSE_OK;
                resp.json_payload = "{\"status\": \"SHUTTING_DOWN\"}";
                SocketUtil::sendMessage(sock, resp);
                running_ = false;
                break;
            }

            default:
                break;
        }
    }
}

} // namespace dfs
