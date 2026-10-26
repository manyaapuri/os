#include "networking/socket_util.h"
#include "common/logger.h"
#include <mutex>

namespace dfs {

namespace {
    std::once_flag wsa_init_flag;
    bool wsa_initialized = false;
}

bool SocketUtil::initWinsock() {
    std::call_once(wsa_init_flag, []() {
        WSADATA wsaData;
        int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (res == 0) {
            wsa_initialized = true;
        } else {
            LOG_ERROR("Winsock", "WSAStartup failed with error: " + std::to_string(res));
        }
    });
    return wsa_initialized;
}

void SocketUtil::cleanupWinsock() {
    if (wsa_initialized) {
        WSACleanup();
        wsa_initialized = false;
    }
}

bool SocketUtil::setTimeouts(SOCKET sock, int timeout_ms) {
    DWORD timeout = static_cast<DWORD>(timeout_ms);
    int res1 = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    int res2 = setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    return (res1 == 0 && res2 == 0);
}

bool SocketUtil::setNoDelay(SOCKET sock, bool nodelay) {
    int flag = nodelay ? 1 : 0;
    return setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag)) == 0;
}

bool SocketUtil::setReuseAddr(SOCKET sock, bool reuse) {
    int flag = reuse ? 1 : 0;
    return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&flag, sizeof(flag)) == 0;
}

bool SocketUtil::sendExact(SOCKET sock, const void* data, size_t len) {
    const char* ptr = static_cast<const char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        int sent = send(sock, ptr, static_cast<int>(remaining), 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            return false;
        }
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

bool SocketUtil::recvExact(SOCKET sock, void* data, size_t len) {
    char* ptr = static_cast<char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        int recvd = recv(sock, ptr, static_cast<int>(remaining), 0);
        if (recvd == SOCKET_ERROR || recvd == 0) {
            return false;
        }
        ptr += recvd;
        remaining -= recvd;
    }
    return true;
}

bool SocketUtil::sendMessage(SOCKET sock, const DfsMessage& msg) {
    ProtocolHeader hdr;
    hdr.magic = DFS_PROTOCOL_MAGIC;
    hdr.version = DFS_PROTOCOL_VERSION;
    hdr.type = static_cast<uint16_t>(msg.type);
    hdr.json_len = static_cast<uint32_t>(msg.json_payload.size());
    hdr.data_len = static_cast<uint64_t>(msg.binary_data.size());

    if (!sendExact(sock, &hdr, sizeof(hdr))) {
        return false;
    }
    if (hdr.json_len > 0) {
        if (!sendExact(sock, msg.json_payload.data(), hdr.json_len)) {
            return false;
        }
    }
    if (hdr.data_len > 0) {
        if (!sendExact(sock, msg.binary_data.data(), static_cast<size_t>(hdr.data_len))) {
            return false;
        }
    }
    return true;
}

bool SocketUtil::recvMessage(SOCKET sock, DfsMessage& msg) {
    ProtocolHeader hdr;
    if (!recvExact(sock, &hdr, sizeof(hdr))) {
        return false;
    }

    if (hdr.magic != DFS_PROTOCOL_MAGIC) {
        LOG_WARN("SocketUtil", "Protocol magic mismatch: " + std::to_string(hdr.magic));
        return false;
    }

    msg.type = static_cast<MessageType>(hdr.type);

    if (hdr.json_len > 0) {
        std::vector<char> json_buf(hdr.json_len);
        if (!recvExact(sock, json_buf.data(), hdr.json_len)) {
            return false;
        }
        msg.json_payload.assign(json_buf.data(), hdr.json_len);
    } else {
        msg.json_payload.clear();
    }

    if (hdr.data_len > 0) {
        if (hdr.data_len > 1024ULL * 1024 * 1024) { // 1GB sanity safety limit
            LOG_ERROR("SocketUtil", "Oversized binary payload rejected: " + std::to_string(hdr.data_len));
            return false;
        }
        msg.binary_data.resize(static_cast<size_t>(hdr.data_len));
        if (!recvExact(sock, msg.binary_data.data(), static_cast<size_t>(hdr.data_len))) {
            return false;
        }
    } else {
        msg.binary_data.clear();
    }

    return true;
}

std::string SocketUtil::getSocketErrorStr(int err) {
    char* s = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   (LPSTR)&s, 0, NULL);
    std::string ret = s ? s : ("Error code " + std::to_string(err));
    LocalFree(s);
    return ret;
}

} // namespace dfs
