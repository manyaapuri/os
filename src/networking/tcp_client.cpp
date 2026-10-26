#include "networking/tcp_client.h"
#include "common/logger.h"

namespace dfs {

TcpClient::TcpClient() {
    SocketUtil::initWinsock();
}

TcpClient::~TcpClient() {
    disconnect();
}

bool TcpClient::connect(const std::string& host, int port, int timeout_ms) {
    disconnect();
    host_ = host;
    port_ = port;

    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == INVALID_SOCKET) {
        LOG_ERROR("TcpClient", "Failed to create socket: " + SocketUtil::getSocketErrorStr(WSAGetLastError()));
        return false;
    }

    SocketUtil::setNoDelay(sock_, true);
    SocketUtil::setTimeouts(sock_, timeout_ms);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr);

    // Non-blocking connect with select timeout
    u_long non_blocking = 1;
    ioctlsocket(sock_, FIONBIO, &non_blocking);

    int res = ::connect(sock_, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
    if (res == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            disconnect();
            return false;
        }

        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(sock_, &write_fds);

        timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int sel_res = select(0, NULL, &write_fds, NULL, &tv);
        if (sel_res <= 0) {
            disconnect();
            return false;
        }
    }

    // Set back to blocking mode with socket timeouts
    non_blocking = 0;
    ioctlsocket(sock_, FIONBIO, &non_blocking);
    SocketUtil::setTimeouts(sock_, timeout_ms);

    return true;
}

void TcpClient::disconnect() {
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

bool TcpClient::isConnected() const {
    return sock_ != INVALID_SOCKET;
}

bool TcpClient::send(const DfsMessage& request) {
    if (!isConnected()) return false;
    return SocketUtil::sendMessage(sock_, request);
}

bool TcpClient::receive(DfsMessage& response) {
    if (!isConnected()) return false;
    return SocketUtil::recvMessage(sock_, response);
}

bool TcpClient::request(const DfsMessage& request, DfsMessage& response) {
    if (!send(request)) return false;
    return receive(response);
}

} // namespace dfs
