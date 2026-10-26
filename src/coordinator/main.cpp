#include "coordinator/coordinator.h"
#include "common/logger.h"
#include <windows.h>
#include <iostream>
#include <memory>

std::unique_ptr<dfs::Coordinator> g_coord;

BOOL WINAPI ConsoleHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        LOG_INFO("CoordinatorMain", "Shutdown event received. Stopping coordinator gracefully...");
        if (g_coord) g_coord->stop();
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char* argv[]) {
    dfs::Coordinator::Config config;
    config.rpc_port = 9001;
    config.web_port = 8080;
    config.db_path = "data/dfs_metadata.db";
    config.static_dir = "dashboard";
    config.default_replication_factor = 2;
    config.storage_node_exe = "dfs_storage_node.exe";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--web-port" && i + 1 < argc) {
            config.web_port = std::stoi(argv[++i]);
        } else if (arg == "--rpc-port" && i + 1 < argc) {
            config.rpc_port = std::stoi(argv[++i]);
        } else if (arg == "--db" && i + 1 < argc) {
            config.db_path = argv[++i];
        } else if (arg == "--dashboard-dir" && i + 1 < argc) {
            config.static_dir = argv[++i];
        } else if (arg == "--node-exe" && i + 1 < argc) {
            config.storage_node_exe = argv[++i];
        }
    }

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    LOG_INFO("CoordinatorMain", "================================================================================");
    LOG_INFO("CoordinatorMain", " Advanced Distributed File System with OS-Level Resource Management & Sync");
    LOG_INFO("CoordinatorMain", " Native Windows OS Demonstration (Winsock2 / Win32 / C++20 / SQLite3)");
    LOG_INFO("CoordinatorMain", "================================================================================");

    g_coord = std::make_unique<dfs::Coordinator>(config);
    if (!g_coord->start()) {
        LOG_ERROR("CoordinatorMain", "Failed to start coordinator.");
        return 1;
    }

    LOG_INFO("CoordinatorMain", "Cluster coordinator running.");
    LOG_INFO("CoordinatorMain", "Open dashboard in your browser: http://127.0.0.1:" + std::to_string(config.web_port) + "/");
    LOG_INFO("CoordinatorMain", "Press Ctrl+C to terminate cluster gracefully.");

    while (g_coord->isRunning()) {
        Sleep(1000);
    }

    return 0;
}
