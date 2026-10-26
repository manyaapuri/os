#include "common/logger.h"
#include "common/json_helper.h"
#include "networking/socket_util.h"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>

namespace dfs {

class SimpleHttpClient {
public:
    SimpleHttpClient(const std::string& host = "127.0.0.1", int port = 8080)
        : host_(host), port_(port) {
        SocketUtil::initWinsock();
    }

    struct Response {
        int status_code = 0;
        std::string body;
    };

    Response request(const std::string& method, const std::string& path, const std::string& body = "", const std::string& content_type = "application/json") {
        Response resp;
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return resp;

        SocketUtil::setNoDelay(sock, true);
        SocketUtil::setTimeouts(sock, 5000);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port_));
        inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);

        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            closesocket(sock);
            return resp;
        }

        std::ostringstream req_ss;
        req_ss << method << " " << path << " HTTP/1.1\r\n";
        req_ss << "Host: " << host_ << ":" << port_ << "\r\n";
        req_ss << "Content-Type: " << content_type << "\r\n";
        req_ss << "Content-Length: " << body.size() << "\r\n";
        req_ss << "Connection: close\r\n\r\n";
        req_ss << body;

        std::string req_str = req_ss.str();
        SocketUtil::sendExact(sock, req_str.data(), req_str.size());

        // Read response
        std::vector<char> buffer(65536);
        std::string full_response;
        int bytes = 0;
        while ((bytes = recv(sock, buffer.data(), static_cast<int>(buffer.size()), 0)) > 0) {
            full_response.append(buffer.data(), bytes);
        }
        closesocket(sock);

        // Parse status code & body
        size_t header_end = full_response.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            std::string header = full_response.substr(0, header_end);
            resp.body = full_response.substr(header_end + 4);

            std::istringstream h_stream(header);
            std::string http_ver;
            h_stream >> http_ver >> resp.status_code;
        }

        return resp;
    }

private:
    std::string host_;
    int port_;
};

} // namespace dfs

void printHelp() {
    std::cout << "\n=======================================================\n";
    std::cout << " Advanced Distributed File System (DFS) - Client CLI\n";
    std::cout << "=======================================================\n";
    std::cout << "Usage:\n";
    std::cout << "  dfs_client upload <local_file> <dfs_path> [user] [repl_factor]\n";
    std::cout << "  dfs_client download <dfs_path> <local_file> [user]\n";
    std::cout << "  dfs_client list [prefix]\n";
    std::cout << "  dfs_client search <query>\n";
    std::cout << "  dfs_client delete <dfs_path> [user]\n";
    std::cout << "  dfs_client rename <old_path> <new_path> [user]\n";
    std::cout << "  dfs_client versions <dfs_path>\n";
    std::cout << "  dfs_client restore <dfs_path> <version>\n";
    std::cout << "  dfs_client nodes\n";
    std::cout << "  dfs_client status\n";
    std::cout << "  dfs_client buffer\n";
    std::cout << "  dfs_client cache\n";
    std::cout << "  dfs_client locks\n";
    std::cout << "  dfs_client scheduler\n";
    std::cout << "  dfs_client disksim\n";
    std::cout << "=======================================================\n\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printHelp();
        return 0;
    }

    std::string command = argv[1];
    dfs::SimpleHttpClient client("127.0.0.1", 8080);

    if (command == "status") {
        auto resp = client.request("GET", "/api/status");
        std::cout << "Cluster Status:\n" << resp.body << "\n";
    }
    else if (command == "nodes") {
        auto resp = client.request("GET", "/api/nodes");
        std::cout << "Storage Nodes:\n" << resp.body << "\n";
    }
    else if (command == "list") {
        auto resp = client.request("GET", "/api/files");
        std::cout << "Files:\n" << resp.body << "\n";
    }
    else if (command == "search" && argc >= 3) {
        std::string query = argv[2];
        auto resp = client.request("GET", "/api/files");
        std::cout << "Search results for: " << query << "\n" << resp.body << "\n";
    }
    else if (command == "upload" && argc >= 4) {
        std::string local_file = argv[2];
        std::string dfs_path = argv[3];
        std::string user = (argc >= 5) ? argv[4] : "admin";
        int repl = (argc >= 6) ? std::stoi(argv[5]) : 2;

        std::ifstream file(local_file, std::ios::binary);
        if (!file) {
            std::cerr << "Error: Could not open local file " << local_file << "\n";
            return 1;
        }
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        dfs::JsonValue j;
        j["path"] = dfs_path;
        j["content"] = content;
        j["owner"] = user;
        j["replication_factor"] = repl;

        auto resp = client.request("POST", "/api/files/upload", j.serialize());
        std::cout << "Upload response (" << resp.status_code << "): " << resp.body << "\n";
    }
    else if (command == "download" && argc >= 4) {
        std::string dfs_path = argv[2];
        std::string local_file = argv[3];

        auto resp = client.request("GET", "/api/files/download?path=" + dfs_path);
        if (resp.status_code == 200) {
            std::ofstream out(local_file, std::ios::binary);
            out.write(resp.body.data(), resp.body.size());
            std::cout << "Successfully downloaded " << resp.body.size() << " bytes to " << local_file << "\n";
        } else {
            std::cerr << "Download failed (" << resp.status_code << "): " << resp.body << "\n";
        }
    }
    else if (command == "delete" && argc >= 3) {
        std::string dfs_path = argv[2];
        auto resp = client.request("DELETE", "/api/files?path=" + dfs_path);
        std::cout << "Delete response: " << resp.body << "\n";
    }
    else if (command == "rename" && argc >= 4) {
        std::string old_p = argv[2];
        std::string new_p = argv[3];
        dfs::JsonValue j;
        j["old_path"] = old_p;
        j["new_path"] = new_p;
        auto resp = client.request("POST", "/api/files/rename", j.serialize());
        std::cout << "Rename response: " << resp.body << "\n";
    }
    else if (command == "versions" && argc >= 3) {
        std::string dfs_path = argv[2];
        auto resp = client.request("GET", "/api/files/versions?path=" + dfs_path);
        std::cout << "Versions:\n" << resp.body << "\n";
    }
    else if (command == "restore" && argc >= 4) {
        std::string dfs_path = argv[2];
        int ver = std::stoi(argv[3]);
        dfs::JsonValue j;
        j["path"] = dfs_path;
        j["version"] = ver;
        auto resp = client.request("POST", "/api/files/restore", j.serialize());
        std::cout << "Restore response: " << resp.body << "\n";
    }
    else if (command == "buffer") {
        auto resp = client.request("GET", "/api/buffer");
        std::cout << "Buffer Stats:\n" << resp.body << "\n";
    }
    else if (command == "cache") {
        auto resp = client.request("GET", "/api/cache");
        std::cout << "Cache Stats:\n" << resp.body << "\n";
    }
    else if (command == "locks") {
        auto resp = client.request("GET", "/api/locks");
        std::cout << "Lock Diagnostics:\n" << resp.body << "\n";
    }
    else if (command == "scheduler") {
        auto resp = client.request("GET", "/api/scheduler");
        std::cout << "Scheduler Stats:\n" << resp.body << "\n";
    }
    else if (command == "disksim") {
        auto resp = client.request("GET", "/api/disk_sim");
        std::cout << "Disk Simulation Comparison:\n" << resp.body << "\n";
    }
    else {
        printHelp();
    }

    return 0;
}
