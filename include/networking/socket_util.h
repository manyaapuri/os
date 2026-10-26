#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include "common/protocol.h"

namespace dfs {

class SocketUtil {
public:
    static bool initWinsock();
    static void cleanupWinsock();

    static bool setTimeouts(SOCKET sock, int timeout_ms);
    static bool setNoDelay(SOCKET sock, bool nodelay = true);
    static bool setReuseAddr(SOCKET sock, bool reuse = true);

    static bool sendExact(SOCKET sock, const void* data, size_t len);
    static bool recvExact(SOCKET sock, void* data, size_t len);

    static bool sendMessage(SOCKET sock, const DfsMessage& msg);
    static bool recvMessage(SOCKET sock, DfsMessage& msg);

    static std::string getSocketErrorStr(int err);
};

} // namespace dfs
