#include "web/http_server.h"
#include "common/logger.h"
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace dfs {

std::string HttpResponse::serialize() const {
    std::ostringstream ss;
    std::string reason = "OK";
    if (status_code == 404) reason = "Not Found";
    else if (status_code == 400) reason = "Bad Request";
    else if (status_code == 403) reason = "Forbidden";
    else if (status_code == 500) reason = "Internal Server Error";

    ss << "HTTP/1.1 " << status_code << " " << reason << "\r\n";
    ss << "Content-Type: " << content_type << "\r\n";
    ss << "Content-Length: " << body.size() << "\r\n";
    ss << "Access-Control-Allow-Origin: *\r\n";
    ss << "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n";
    ss << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
    ss << "Connection: close\r\n";

    for (const auto& [k, v] : headers) {
        ss << k << ": " << v << "\r\n";
    }

    ss << "\r\n";
    ss << body;
    return ss.str();
}

HttpServer::HttpServer(int port, const std::string& static_root_dir)
    : port_(port), static_root_(static_root_dir) {}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
    tcp_server_ = std::make_unique<TcpServer>();
    tcp_server_->setConnectionHandler([this](SOCKET sock, const std::string& ip, int client_port) {
        handleSocket(sock, ip, client_port);
    });

    if (!tcp_server_->start(port_)) {
        LOG_ERROR("HttpServer", "Failed to start HTTP server on port " + std::to_string(port_));
        return false;
    }

    LOG_INFO("HttpServer", "Dashboard & REST API server online at http://127.0.0.1:" + std::to_string(port_) + "/");
    return true;
}

void HttpServer::stop() {
    if (tcp_server_) {
        tcp_server_->stop();
    }
}

bool HttpServer::isRunning() const {
    return tcp_server_ && tcp_server_->isRunning();
}

void HttpServer::registerRoute(const std::string& method, const std::string& path, HttpHandler handler) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    routes_[method + ":" + path] = std::move(handler);
}

HttpRequest HttpServer::parseRequest(const std::string& raw, const std::string& client_ip) {
    HttpRequest req;
    req.client_ip = client_ip;

    std::istringstream stream(raw);
    std::string line;
    if (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream line_stream(line);
        line_stream >> req.method >> req.path;

        size_t q_pos = req.path.find('?');
        if (q_pos != std::string::npos) {
            req.query_string = req.path.substr(q_pos + 1);
            req.path = req.path.substr(0, q_pos);

            std::istringstream qs(req.query_string);
            std::string pair;
            while (std::getline(qs, pair, '&')) {
                size_t eq_pos = pair.find('=');
                if (eq_pos != std::string::npos) {
                    req.query_params[pair.substr(0, eq_pos)] = pair.substr(eq_pos + 1);
                } else if (!pair.empty()) {
                    req.query_params[pair] = "";
                }
            }
        }
    }

    while (std::getline(stream, line) && line != "\r" && !line.empty()) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t col_pos = line.find(':');
        if (col_pos != std::string::npos) {
            std::string key = line.substr(0, col_pos);
            std::string val = line.substr(col_pos + 1);
            while (!val.empty() && val.front() == ' ') val.erase(val.begin());
            req.headers[key] = val;
        }
    }

    std::ostringstream body_ss;
    body_ss << stream.rdbuf();
    req.body = body_ss.str();

    return req;
}

HttpResponse HttpServer::serveStaticFile(const std::string& req_path) {
    HttpResponse resp;
    std::string path = req_path;
    if (path == "/" || path.empty()) {
        path = "/index.html";
    }

    std::string full_path = static_root_ + path;
    if (!std::filesystem::exists(full_path) || std::filesystem::is_directory(full_path)) {
        resp.status_code = 404;
        resp.content_type = "text/plain";
        resp.body = "404 Not Found";
        return resp;
    }

    std::ifstream file(full_path, std::ios::binary);
    if (!file) {
        resp.status_code = 500;
        resp.content_type = "text/plain";
        resp.body = "Error opening file";
        return resp;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    resp.body = ss.str();
    resp.status_code = 200;

    if (full_path.ends_with(".html")) resp.content_type = "text/html; charset=utf-8";
    else if (full_path.ends_with(".css")) resp.content_type = "text/css; charset=utf-8";
    else if (full_path.ends_with(".js")) resp.content_type = "application/javascript; charset=utf-8";
    else if (full_path.ends_with(".json")) resp.content_type = "application/json";
    else if (full_path.ends_with(".svg")) resp.content_type = "image/svg+xml";
    else resp.content_type = "text/plain";

    return resp;
}

void HttpServer::handleSocket(SOCKET sock, const std::string& ip, int port) {
    char buf[16384];
    int recvd = recv(sock, buf, sizeof(buf) - 1, 0);
    if (recvd <= 0) return;
    buf[recvd] = '\0';

    HttpRequest req = parseRequest(std::string(buf, recvd), ip);

    // Support CORS preflight
    if (req.method == "OPTIONS") {
        HttpResponse opt_resp;
        opt_resp.status_code = 204;
        opt_resp.body = "";
        std::string out = opt_resp.serialize();
        SocketUtil::sendExact(sock, out.data(), out.size());
        return;
    }

    HttpResponse resp;
    HttpHandler handler = nullptr;

    {
        std::lock_guard<std::mutex> lock(routes_mutex_);
        auto it = routes_.find(req.method + ":" + req.path);
        if (it != routes_.end()) {
            handler = it->second;
        }
    }

    if (handler) {
        try {
            resp = handler(req);
        } catch (const std::exception& ex) {
            resp.status_code = 500;
            resp.body = "{\"error\": \"Exception in handler: " + std::string(ex.what()) + "\"}";
        }
    } else {
        // Fallback to static file server
        resp = serveStaticFile(req.path);
    }

    std::string out = resp.serialize();
    SocketUtil::sendExact(sock, out.data(), out.size());
}

} // namespace dfs
