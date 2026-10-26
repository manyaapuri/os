#include "storage_node/storage_node.h"
#include "common/logger.h"
#include <windows.h>
#include <iostream>
#include <string>

std::unique_ptr<dfs::StorageNode> g_node;

BOOL WINAPI ConsoleHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        LOG_INFO("StorageNodeMain", "Shutdown signal received. Stopping node...");
        if (g_node) g_node->stop();
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char* argv[]) {
    int node_id = 1;
    int port = 9101;
    std::string storage_dir = "storage/node1";
    int coordinator_port = 9001;
    std::string coordinator_host = "127.0.0.1";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--id" && i + 1 < argc) {
            node_id = std::stoi(argv[++i]);
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--dir" && i + 1 < argc) {
            storage_dir = argv[++i];
        } else if (arg == "--coord-port" && i + 1 < argc) {
            coordinator_port = std::stoi(argv[++i]);
        } else if (arg == "--coord-host" && i + 1 < argc) {
            coordinator_host = argv[++i];
        }
    }

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    LOG_INFO("StorageNodeMain", "Launching Storage Node " + std::to_string(node_id) +
             " on port " + std::to_string(port) + " (Storage: " + storage_dir + ")");

    g_node = std::make_unique<dfs::StorageNode>(node_id, port, storage_dir, coordinator_port, coordinator_host);
    if (!g_node->start()) {
        LOG_ERROR("StorageNodeMain", "Failed to start Storage Node.");
        return 1;
    }

    // Keep process alive while node is running
    while (g_node->isRunning()) {
        Sleep(500);
    }

    return 0;
}
