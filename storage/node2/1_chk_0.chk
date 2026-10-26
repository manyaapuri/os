cmake_minimum_required(VERSION 3.20)
project(AdvancedDistributedFileSystem LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /EHsc /utf-8)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    add_definitions(-DWIN32_LEAN_AND_MEAN)
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite
)

# SQLite library
add_library(sqlite3 STATIC
    third_party/sqlite/sqlite3.c
)
if(NOT MSVC)
    target_compile_options(sqlite3 PRIVATE -w)
endif()

# Core DFS library
add_library(dfs_core STATIC
    src/common/sha256.cpp
    src/networking/socket_util.cpp
    src/networking/tcp_client.cpp
    src/networking/tcp_server.cpp
    src/metadata/metadata_manager.cpp
    src/scheduler/request_scheduler.cpp
    src/sync/lock_manager.cpp
    src/recovery/wal_manager.cpp
    src/process/process_manager.cpp
    src/disk/disk_scheduler.cpp
    src/security/security_manager.cpp
    src/web/http_server.cpp
    src/coordinator/coordinator.cpp
    src/storage_node/storage_node.cpp
)

target_link_libraries(dfs_core PUBLIC
    sqlite3
    ws2_32
    psapi
)

# 1. Coordinator Executable
add_executable(dfs_coordinator
    src/coordinator/main.cpp
)
target_link_libraries(dfs_coordinator PRIVATE dfs_core)

# 2. Storage Node Executable
add_executable(dfs_storage_node
    src/storage_node/main.cpp
)
target_link_libraries(dfs_storage_node PRIVATE dfs_core)

# 3. Client CLI Executable
add_executable(dfs_client
    src/client/main.cpp
)
target_link_libraries(dfs_client PRIVATE dfs_core)

# 4. Automated Tests Executable
add_executable(dfs_tests
    src/tests/test_main.cpp
)
target_link_libraries(dfs_tests PRIVATE dfs_core)

# Copy dashboard and config to build directory after build
add_custom_command(TARGET dfs_coordinator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/dashboard
        $<TARGET_FILE_DIR:dfs_coordinator>/dashboard
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/config
        $<TARGET_FILE_DIR:dfs_coordinator>/config
)
