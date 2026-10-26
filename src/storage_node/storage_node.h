#pragma once

#include "networking/tcp_server.h"
#include "networking/tcp_client.h"
#include "buffer/bounded_buffer.h"
#include "common/types.h"
#include <string>
#include <atomic>
#include <thread>
#include <memory>

namespace dfs {

class StorageNode {
public:
    StorageNode(int node_id, int port, const std::string& storage_dir,
                int coordinator_port = 9001, const std::string& coordinator_host = "127.0.0.1");
    ~StorageNode();

    bool start();
    void stop();
    bool isRunning() const { return running_; }

    int getId() const { return node_id_; }
    int getPort() const { return port_; }
    std::string getStorageDir() const { return storage_dir_; }

    uint64_t getUsedSpace() const;
    uint64_t getTotalSpace() const;

private:
    void handleClient(SOCKET sock, const std::string& ip, int port);
    void heartbeatLoop();

    int node_id_;
    int port_;
    std::string storage_dir_;
    int coordinator_port_;
    std::string coordinator_host_;

    std::atomic<bool> running_{false};
    std::unique_ptr<TcpServer> server_;
    std::thread heartbeat_thread_;

    ChunkBuffer chunk_buffer_;
};

} // namespace dfs
