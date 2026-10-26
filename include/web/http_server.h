#pragma once

#include "networking/tcp_server.h"
#include <string>
#include <map>
#include <functional>
#include <memory>

namespace dfs {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query_string;
    std::map<std::string, std::string> query_params;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string client_ip;
};

struct HttpResponse {
    int status_code = 200;
    std::string content_type = "application/json";
    std::map<std::string, std::string> headers;
    std::string body;

    std::string serialize() const;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
public:
    HttpServer(int port, const std::string& static_root_dir = "dashboard");
    ~HttpServer();

    bool start();
    void stop();
    bool isRunning() const;

    void registerRoute(const std::string& method, const std::string& path, HttpHandler handler);

private:
    void handleSocket(SOCKET sock, const std::string& ip, int port);
    HttpRequest parseRequest(const std::string& raw, const std::string& client_ip);
    HttpResponse serveStaticFile(const std::string& path);

    int port_;
    std::string static_root_;
    std::unique_ptr<TcpServer> tcp_server_;
    std::map<std::string, HttpHandler> routes_; // key: "METHOD:PATH"
    std::mutex routes_mutex_;
};

} // namespace dfs
