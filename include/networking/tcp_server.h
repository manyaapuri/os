#pragma once

#include "networking/socket_util.h"
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>

namespace dfs {

using ConnectionHandler = std::function<void(SOCKET client_sock, const std::string& client_ip, int client_port)>;

class TcpServer {
public:
    TcpServer();
    ~TcpServer();

    bool start(int port, const std::string& host = "0.0.0.0");
    void stop();
    bool isRunning() const { return running_; }
    int getPort() const { return port_; }

    void setConnectionHandler(ConnectionHandler handler) {
        handler_ = std::move(handler);
    }

private:
    void acceptLoop();

    SOCKET listen_sock_ = INVALID_SOCKET;
    int port_ = 0;
    std::string host_;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    ConnectionHandler handler_;

    std::mutex client_threads_mutex_;
    std::vector<std::thread> client_threads_;
};

} // namespace dfs
