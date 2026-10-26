#include "networking/tcp_server.h"
#include "common/logger.h"

namespace dfs {

TcpServer::TcpServer() {
    SocketUtil::initWinsock();
}

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start(int port, const std::string& host) {
    if (running_) return true;

    port_ = port;
    host_ = host;

    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCKET) {
        LOG_ERROR("TcpServer", "Failed to create listen socket: " + SocketUtil::getSocketErrorStr(WSAGetLastError()));
        return false;
    }

    SocketUtil::setReuseAddr(listen_sock_, true);
    SocketUtil::setNoDelay(listen_sock_, true);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    if (host == "0.0.0.0" || host.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    }

    if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        LOG_ERROR("TcpServer", "Bind failed on port " + std::to_string(port) + ": " + SocketUtil::getSocketErrorStr(WSAGetLastError()));
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        return false;
    }

    if (listen(listen_sock_, SOMAXCONN) == SOCKET_ERROR) {
        LOG_ERROR("TcpServer", "Listen failed on port " + std::to_string(port) + ": " + SocketUtil::getSocketErrorStr(WSAGetLastError()));
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread(&TcpServer::acceptLoop, this);
    LOG_INFO("TcpServer", "Server listening on " + host + ":" + std::to_string(port));
    return true;
}

void TcpServer::stop() {
    if (!running_) return;
    running_ = false;

    if (listen_sock_ != INVALID_SOCKET) {
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(client_threads_mutex_);
        for (auto& t : client_threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        client_threads_.clear();
    }
    LOG_INFO("TcpServer", "Server stopped on port " + std::to_string(port_));
}

void TcpServer::acceptLoop() {
    while (running_) {
        sockaddr_in client_addr{};
        int client_addr_len = sizeof(client_addr);
        SOCKET client_sock = accept(listen_sock_, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);

        if (client_sock == INVALID_SOCKET) {
            if (!running_) break;
            int err = WSAGetLastError();
            if (err == WSAEINTR || err == WSAENOTSOCK) break;
            continue;
        }

        SocketUtil::setTimeouts(client_sock, 10000);
        SocketUtil::setNoDelay(client_sock, true);

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
        int client_port = ntohs(client_addr.sin_port);

        if (handler_) {
            std::lock_guard<std::mutex> lock(client_threads_mutex_);
            // Cleanup finished threads
            for (auto it = client_threads_.begin(); it != client_threads_.end(); ) {
                if (!it->joinable()) {
                    it = client_threads_.erase(it);
                } else {
                    ++it;
                }
            }

            client_threads_.emplace_back([this, client_sock, ip = std::string(ip_str), client_port]() {
                handler_(client_sock, ip, client_port);
                closesocket(client_sock);
            });
            // Detach thread to prevent resource leak and unbounded vector growth
            client_threads_.back().detach();
            client_threads_.pop_back();
        } else {
            closesocket(client_sock);
        }
    }
}

} // namespace dfs
