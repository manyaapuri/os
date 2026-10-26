#pragma once

#include "networking/socket_util.h"
#include <string>
#include <memory>

namespace dfs {

class TcpClient {
public:
    TcpClient();
    ~TcpClient();

    bool connect(const std::string& host, int port, int timeout_ms = 3000);
    void disconnect();
    bool isConnected() const;

    bool send(const DfsMessage& request);
    bool receive(DfsMessage& response);
    bool request(const DfsMessage& request, DfsMessage& response);

    SOCKET getSocket() const { return sock_; }

private:
    SOCKET sock_ = INVALID_SOCKET;
    std::string host_;
    int port_ = 0;
};

} // namespace dfs
