#include "metadata/metadata_manager.h"
#include "common/logger.h"
#include "sqlite3.h"
#include <chrono>
#include <filesystem>

namespace dfs {

MetadataManager::MetadataManager() = default;

MetadataManager::~MetadataManager() {
    close();
}

bool MetadataManager::init(const std::string& db_path) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    db_path_ = db_path;

    // Ensure parent directory exists
    std::filesystem::path p(db_path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }

    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        LOG_ERROR("Metadata", "Failed to open SQLite database: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }

    // Enable WAL mode in SQLite for high concurrency and crash durability
    char* err_msg = nullptr;
    sqlite3_exec(db_, "PRAGMA journal_mode = WAL;", nullptr, nullptr, &err_msg);
    sqlite3_exec(db_, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, &err_msg);
    sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, &err_msg);
    if (err_msg) sqlite3_free(err_msg);

    if (!createTables()) {
        LOG_ERROR("Metadata", "Failed to initialize database tables");
        return false;
    }

    initDefaultUsers();
    LOG_INFO("Metadata", "SQLite metadata initialized at " + db_path_);
    return true;
}

void MetadataManager::close() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool MetadataManager::executeSql(const std::string& sql) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string err = err_msg ? err_msg : "Unknown error";
        LOG_ERROR("Metadata", "SQL execution failed: " + err + " | Query: " + sql);
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool MetadataManager::createTables() {
    const char* schema = R"(
        CREATE TABLE IF NOT EXISTS files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            path TEXT UNIQUE NOT NULL,
            filename TEXT NOT NULL,
            size INTEGER DEFAULT 0,
            owner TEXT DEFAULT 'admin',
            permissions INTEGER DEFAULT 493,
            version INTEGER DEFAULT 1,
            checksum_sha256 TEXT,
            replication_factor INTEGER DEFAULT 2,
            created_at INTEGER,
            updated_at INTEGER,
            is_deleted INTEGER DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS chunks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            file_id INTEGER NOT NULL,
            chunk_index INTEGER NOT NULL,
            chunk_id TEXT UNIQUE NOT NULL,
            offset INTEGER NOT NULL,
            size INTEGER NOT NULL,
            checksum_sha256 TEXT,
            FOREIGN KEY(file_id) REFERENCES files(id) ON DELETE CASCADE
        );

        CREATE TABLE IF NOT EXISTS chunk_replicas (
            chunk_id TEXT NOT NULL,
            node_id INTEGER NOT NULL,
            PRIMARY KEY(chunk_id, node_id)
        );

        CREATE TABLE IF NOT EXISTS file_versions (
            version_id INTEGER PRIMARY KEY AUTOINCREMENT,
            file_id INTEGER NOT NULL,
            path TEXT NOT NULL,
            version INTEGER NOT NULL,
            size INTEGER NOT NULL,
            checksum_sha256 TEXT,
            created_at INTEGER,
            created_by TEXT,
            FOREIGN KEY(file_id) REFERENCES files(id) ON DELETE CASCADE
        );

        CREATE TABLE IF NOT EXISTS storage_nodes (
            id INTEGER PRIMARY KEY,
            name TEXT NOT NULL,
            host TEXT DEFAULT '127.0.0.1',
            port INTEGER NOT NULL,
            storage_path TEXT,
            total_space INTEGER DEFAULT 0,
            used_space INTEGER DEFAULT 0,
            status TEXT DEFAULT 'OFFLINE',
            process_id INTEGER DEFAULT 0,
            last_heartbeat INTEGER DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS audit_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp INTEGER NOT NULL,
            user TEXT NOT NULL,
            action TEXT NOT NULL,
            target_path TEXT,
            success INTEGER DEFAULT 1,
            details TEXT,
            client_ip TEXT
        );

        CREATE TABLE IF NOT EXISTS wal_logs (
            lsn INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp INTEGER NOT NULL,
            tx_id TEXT NOT NULL,
            operation TEXT NOT NULL,
            file_path TEXT,
            state TEXT DEFAULT 'PENDING',
            payload TEXT
        );

        CREATE TABLE IF NOT EXISTS users (
            username TEXT PRIMARY KEY,
            password_hash TEXT NOT NULL,
            role TEXT DEFAULT 'user',
            created_at INTEGER
        );

        CREATE INDEX IF NOT EXISTS idx_files_path ON files(path);
        CREATE INDEX IF NOT EXISTS idx_chunks_file ON chunks(file_id);
        CREATE INDEX IF NOT EXISTS idx_audit_time ON audit_logs(timestamp);
    )";
    return executeSql(schema);
}

bool MetadataManager::initDefaultUsers() {
    const char* sql = "INSERT OR IGNORE INTO users (username, password_hash, role, created_at) "
                      "VALUES ('admin', 'admin123', 'admin', 0), ('guest', 'guest123', 'user', 0);";
    return executeSql(sql);
}

bool MetadataManager::createFile(FileMetadata& file) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "INSERT INTO files (path, filename, size, owner, permissions, version, "
                      "checksum_sha256, replication_factor, created_at, updated_at, is_deleted) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (file.created_at == 0) file.created_at = now;
    file.updated_at = now;

    sqlite3_bind_text(stmt, 1, file.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, file.filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, file.size);
    sqlite3_bind_text(stmt, 4, file.owner.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, file.permissions);
    sqlite3_bind_int(stmt, 6, file.version);
    sqlite3_bind_text(stmt, 7, file.checksum_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 8, file.replication_factor);
    sqlite3_bind_int64(stmt, 9, file.created_at);
    sqlite3_bind_int64(stmt, 10, file.updated_at);
    sqlite3_bind_int(stmt, 11, file.is_deleted ? 1 : 0);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (ok) {
        file.id = static_cast<int>(sqlite3_last_insert_rowid(db_));
    }
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<FileMetadata> MetadataManager::getFileByPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "SELECT id, path, filename, size, owner, permissions, version, checksum_sha256, "
                      "replication_factor, created_at, updated_at, is_deleted FROM files WHERE path = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;

    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<FileMetadata> res;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        FileMetadata f;
        f.id = sqlite3_column_int(stmt, 0);
        f.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        f.filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        f.size = sqlite3_column_int64(stmt, 3);
        f.owner = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        f.permissions = sqlite3_column_int(stmt, 5);
        f.version = sqlite3_column_int(stmt, 6);
        const char* csum = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        f.checksum_sha256 = csum ? csum : "";
        f.replication_factor = sqlite3_column_int(stmt, 8);
        f.created_at = sqlite3_column_int64(stmt, 9);
        f.updated_at = sqlite3_column_int64(stmt, 10);
        f.is_deleted = (sqlite3_column_int(stmt, 11) == 1);
        res = f;
    }
    sqlite3_finalize(stmt);

    if (res) {
        res->chunks = getChunksForFile(res->id);
    }
    return res;
}

std::optional<FileMetadata> MetadataManager::getFileById(int id) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "SELECT id, path, filename, size, owner, permissions, version, checksum_sha256, "
                      "replication_factor, created_at, updated_at, is_deleted FROM files WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;

    sqlite3_bind_int(stmt, 1, id);
    std::optional<FileMetadata> res;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        FileMetadata f;
        f.id = sqlite3_column_int(stmt, 0);
        f.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        f.filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        f.size = sqlite3_column_int64(stmt, 3);
        f.owner = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        f.permissions = sqlite3_column_int(stmt, 5);
        f.version = sqlite3_column_int(stmt, 6);
        const char* csum = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        f.checksum_sha256 = csum ? csum : "";
        f.replication_factor = sqlite3_column_int(stmt, 8);
        f.created_at = sqlite3_column_int64(stmt, 9);
        f.updated_at = sqlite3_column_int64(stmt, 10);
        f.is_deleted = (sqlite3_column_int(stmt, 11) == 1);
        res = f;
    }
    sqlite3_finalize(stmt);

    if (res) {
        res->chunks = getChunksForFile(res->id);
    }
    return res;
}

bool MetadataManager::updateFile(const FileMetadata& file) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "UPDATE files SET size = ?, owner = ?, permissions = ?, version = ?, "
                      "checksum_sha256 = ?, replication_factor = ?, updated_at = ?, is_deleted = ? "
                      "WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    sqlite3_bind_int64(stmt, 1, file.size);
    sqlite3_bind_text(stmt, 2, file.owner.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, file.permissions);
    sqlite3_bind_int(stmt, 4, file.version);
    sqlite3_bind_text(stmt, 5, file.checksum_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, file.replication_factor);
    sqlite3_bind_int64(stmt, 7, now);
    sqlite3_bind_int(stmt, 8, file.is_deleted ? 1 : 0);
    sqlite3_bind_int(stmt, 9, file.id);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MetadataManager::renameFile(const std::string& old_path, const std::string& new_path) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::filesystem::path p(new_path);
    std::string filename = p.filename().string();

    const char* sql = "UPDATE files SET path = ?, filename = ? WHERE path = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR("Metadata", "Failed to prepare rename SQL: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }

    sqlite3_bind_text(stmt, 1, new_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, old_path.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        LOG_ERROR("Metadata", "Failed to execute rename: " + std::string(sqlite3_errmsg(db_)));
        sqlite3_finalize(stmt);
        return false;
    }
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

bool MetadataManager::deleteFile(const std::string& path, bool soft_delete) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (soft_delete) {
        const char* sql = "UPDATE files SET is_deleted = 1 WHERE path = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return ok;
    } else {
        const char* sql = "DELETE FROM files WHERE path = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return ok;
    }
}

std::vector<FileMetadata> MetadataManager::listFiles(const std::string& prefix, bool include_deleted) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::string sql = "SELECT id, path, filename, size, owner, permissions, version, checksum_sha256, "
                      "replication_factor, created_at, updated_at, is_deleted FROM files WHERE 1=1";
    if (!include_deleted) {
        sql += " AND is_deleted = 0";
    }
    if (!prefix.empty()) {
        sql += " AND path LIKE '" + prefix + "%'";
    }
    sql += " ORDER BY path ASC;";

    sqlite3_stmt* stmt = nullptr;
    std::vector<FileMetadata> list;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return list;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FileMetadata f;
        f.id = sqlite3_column_int(stmt, 0);
        f.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        f.filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        f.size = sqlite3_column_int64(stmt, 3);
        f.owner = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        f.permissions = sqlite3_column_int(stmt, 5);
        f.version = sqlite3_column_int(stmt, 6);
        const char* csum = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        f.checksum_sha256 = csum ? csum : "";
        f.replication_factor = sqlite3_column_int(stmt, 8);
        f.created_at = sqlite3_column_int64(stmt, 9);
        f.updated_at = sqlite3_column_int64(stmt, 10);
        f.is_deleted = (sqlite3_column_int(stmt, 11) == 1);
        list.push_back(f);
    }
    sqlite3_finalize(stmt);

    for (auto& file : list) {
        file.chunks = getChunksForFile(file.id);
    }
    return list;
}

std::vector<FileMetadata> MetadataManager::searchFiles(const std::string& query) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::string pattern = "%" + query + "%";
    const char* sql = "SELECT id, path, filename, size, owner, permissions, version, checksum_sha256, "
                      "replication_factor, created_at, updated_at, is_deleted FROM files "
                      "WHERE is_deleted = 0 AND (path LIKE ? OR filename LIKE ?) ORDER BY path ASC;";

    sqlite3_stmt* stmt = nullptr;
    std::vector<FileMetadata> list;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FileMetadata f;
        f.id = sqlite3_column_int(stmt, 0);
        f.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        f.filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        f.size = sqlite3_column_int64(stmt, 3);
        f.owner = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        f.permissions = sqlite3_column_int(stmt, 5);
        f.version = sqlite3_column_int(stmt, 6);
        const char* csum = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        f.checksum_sha256 = csum ? csum : "";
        f.replication_factor = sqlite3_column_int(stmt, 8);
        f.created_at = sqlite3_column_int64(stmt, 9);
        f.updated_at = sqlite3_column_int64(stmt, 10);
        f.is_deleted = (sqlite3_column_int(stmt, 11) == 1);
        list.push_back(f);
    }
    sqlite3_finalize(stmt);

    for (auto& file : list) {
        file.chunks = getChunksForFile(file.id);
    }
    return list;
}

bool MetadataManager::addChunk(int file_id, const FileChunk& chunk) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "INSERT OR REPLACE INTO chunks (file_id, chunk_index, chunk_id, offset, size, checksum_sha256) "
                      "VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, file_id);
    sqlite3_bind_int(stmt, 2, chunk.chunk_index);
    sqlite3_bind_text(stmt, 3, chunk.chunk_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, chunk.offset);
    sqlite3_bind_int64(stmt, 5, chunk.size);
    sqlite3_bind_text(stmt, 6, chunk.checksum_sha256.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MetadataManager::assignReplica(const std::string& chunk_id, int node_id) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "INSERT OR IGNORE INTO chunk_replicas (chunk_id, node_id) VALUES (?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, chunk_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, node_id);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MetadataManager::removeReplica(const std::string& chunk_id, int node_id) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "DELETE FROM chunk_replicas WHERE chunk_id = ? AND node_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, chunk_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, node_id);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<FileChunk> MetadataManager::getChunksForFile(int file_id) {
    const char* sql = "SELECT chunk_index, chunk_id, offset, size, checksum_sha256 FROM chunks "
                      "WHERE file_id = ? ORDER BY chunk_index ASC;";
    sqlite3_stmt* stmt = nullptr;
    std::vector<FileChunk> chunks;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return chunks;

    sqlite3_bind_int(stmt, 1, file_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FileChunk chk;
        chk.chunk_index = sqlite3_column_int(stmt, 0);
        chk.chunk_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        chk.offset = sqlite3_column_int64(stmt, 2);
        chk.size = sqlite3_column_int64(stmt, 3);
        const char* csum = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        chk.checksum_sha256 = csum ? csum : "";
        chunks.push_back(chk);
    }
    sqlite3_finalize(stmt);

    for (auto& chk : chunks) {
        chk.replica_node_ids = getReplicasForChunk(chk.chunk_id);
    }
    return chunks;
}

std::vector<int> MetadataManager::getReplicasForChunk(const std::string& chunk_id) {
    const char* sql = "SELECT node_id FROM chunk_replicas WHERE chunk_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    std::vector<int> nodes;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return nodes;

    sqlite3_bind_text(stmt, 1, chunk_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        nodes.push_back(sqlite3_column_int(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return nodes;
}

bool MetadataManager::saveVersionSnapshot(const FileMetadata& file, const std::string& created_by) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "INSERT INTO file_versions (file_id, path, version, size, checksum_sha256, created_at, created_by) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    sqlite3_bind_int(stmt, 1, file.id);
    sqlite3_bind_text(stmt, 2, file.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, file.version);
    sqlite3_bind_int64(stmt, 4, file.size);
    sqlite3_bind_text(stmt, 5, file.checksum_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, now);
    sqlite3_bind_text(stmt, 7, created_by.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<FileVersion> MetadataManager::getVersionsForFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "SELECT version_id, file_id, path, version, size, checksum_sha256, created_at, created_by "
                      "FROM file_versions WHERE path = ? ORDER BY version DESC;";
    sqlite3_stmt* stmt = nullptr;
    std::vector<FileVersion> list;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FileVersion v;
        v.version_id = sqlite3_column_int(stmt, 0);
        v.file_id = sqlite3_column_int(stmt, 1);
        v.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        v.version = sqlite3_column_int(stmt, 3);
        v.size = sqlite3_column_int64(stmt, 4);
        const char* csum = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        v.checksum_sha256 = csum ? csum : "";
        v.created_at = sqlite3_column_int64(stmt, 6);
        const char* by = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        v.created_by = by ? by : "";
        list.push_back(v);
    }
    sqlite3_finalize(stmt);
    return list;
}

std::optional<FileVersion> MetadataManager::getVersion(const std::string& path, int version) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "SELECT version_id, file_id, path, version, size, checksum_sha256, created_at, created_by "
                      "FROM file_versions WHERE path = ? AND version = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;

    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, version);
    std::optional<FileVersion> res;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        FileVersion v;
        v.version_id = sqlite3_column_int(stmt, 0);
        v.file_id = sqlite3_column_int(stmt, 1);
        v.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        v.version = sqlite3_column_int(stmt, 3);
        v.size = sqlite3_column_int64(stmt, 4);
        const char* csum = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        v.checksum_sha256 = csum ? csum : "";
        v.created_at = sqlite3_column_int64(stmt, 6);
        const char* by = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        v.created_by = by ? by : "";
        res = v;
    }
    sqlite3_finalize(stmt);
    return res;
}

bool MetadataManager::restoreVersion(const std::string& path, int target_version) {
    auto v_opt = getVersion(path, target_version);
    if (!v_opt) return false;

    auto f_opt = getFileByPath(path);
    if (!f_opt) return false;

    FileMetadata f = *f_opt;
    saveVersionSnapshot(f, "system-pre-restore");

    f.version += 1;
    f.size = v_opt->size;
    f.checksum_sha256 = v_opt->checksum_sha256;
    f.is_deleted = false;
    return updateFile(f);
}

bool MetadataManager::registerOrUpdateNode(const NodeInfo& node) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "INSERT OR REPLACE INTO storage_nodes "
                      "(id, name, host, port, storage_path, total_space, used_space, status, process_id, last_heartbeat) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    std::string status_str = nodeStatusToString(node.status);
    sqlite3_bind_int(stmt, 1, node.id);
    sqlite3_bind_text(stmt, 2, node.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, node.host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, node.port);
    sqlite3_bind_text(stmt, 5, node.storage_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, node.total_space);
    sqlite3_bind_int64(stmt, 7, node.used_space);
    sqlite3_bind_text(stmt, 8, status_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 9, node.process_id);
    sqlite3_bind_int64(stmt, 10, node.last_heartbeat);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MetadataManager::updateNodeHeartbeat(int node_id, int64_t heartbeat_epoch_sec, uint64_t used_space, uint64_t total_space) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "UPDATE storage_nodes SET last_heartbeat = ?, used_space = ?, total_space = ?, status = 'ONLINE' WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, heartbeat_epoch_sec);
    sqlite3_bind_int64(stmt, 2, used_space);
    sqlite3_bind_int64(stmt, 3, total_space);
    sqlite3_bind_int(stmt, 4, node_id);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MetadataManager::updateNodeStatus(int node_id, NodeStatus status) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "UPDATE storage_nodes SET status = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    std::string s = nodeStatusToString(status);
    sqlite3_bind_text(stmt, 1, s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, node_id);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<NodeInfo> MetadataManager::getAllNodes() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "SELECT id, name, host, port, storage_path, total_space, used_space, status, process_id, last_heartbeat "
                      "FROM storage_nodes ORDER BY id ASC;";
    sqlite3_stmt* stmt = nullptr;
    std::vector<NodeInfo> nodes;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return nodes;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        NodeInfo n;
        n.id = sqlite3_column_int(stmt, 0);
        n.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        n.host = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        n.port = sqlite3_column_int(stmt, 3);
        const char* sp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        n.storage_path = sp ? sp : "";
        n.total_space = sqlite3_column_int64(stmt, 5);
        n.used_space = sqlite3_column_int64(stmt, 6);
        std::string status_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        if (status_str == "ONLINE") n.status = NodeStatus::ONLINE;
        else if (status_str == "DEGRADED") n.status = NodeStatus::DEGRADED;
        else n.status = NodeStatus::OFFLINE;
        n.process_id = sqlite3_column_int(stmt, 8);
        n.last_heartbeat = sqlite3_column_int64(stmt, 9);
        nodes.push_back(n);
    }
    sqlite3_finalize(stmt);
    return nodes;
}

std::optional<NodeInfo> MetadataManager::getNodeById(int node_id) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "SELECT id, name, host, port, storage_path, total_space, used_space, status, process_id, last_heartbeat "
                      "FROM storage_nodes WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;

    sqlite3_bind_int(stmt, 1, node_id);
    std::optional<NodeInfo> res;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        NodeInfo n;
        n.id = sqlite3_column_int(stmt, 0);
        n.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        n.host = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        n.port = sqlite3_column_int(stmt, 3);
        const char* sp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        n.storage_path = sp ? sp : "";
        n.total_space = sqlite3_column_int64(stmt, 5);
        n.used_space = sqlite3_column_int64(stmt, 6);
        std::string status_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        if (status_str == "ONLINE") n.status = NodeStatus::ONLINE;
        else if (status_str == "DEGRADED") n.status = NodeStatus::DEGRADED;
        else n.status = NodeStatus::OFFLINE;
        n.process_id = sqlite3_column_int(stmt, 8);
        n.last_heartbeat = sqlite3_column_int64(stmt, 9);
        res = n;
    }
    sqlite3_finalize(stmt);
    return res;
}

bool MetadataManager::addAuditLog(const std::string& user, const std::string& action, const std::string& target_path,
                                  bool success, const std::string& details, const std::string& client_ip) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "INSERT INTO audit_logs (timestamp, user, action, target_path, success, details, client_ip) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_text(stmt, 2, user.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, action.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, target_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, success ? 1 : 0);
    sqlite3_bind_text(stmt, 6, details.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, client_ip.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<AuditLogEntry> MetadataManager::getRecentAuditLogs(int limit) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "SELECT id, timestamp, user, action, target_path, success, details, client_ip "
                      "FROM audit_logs ORDER BY id DESC LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    std::vector<AuditLogEntry> list;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AuditLogEntry entry;
        entry.id = sqlite3_column_int(stmt, 0);
        entry.timestamp = sqlite3_column_int64(stmt, 1);
        entry.user = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        entry.action = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* tp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        entry.target_path = tp ? tp : "";
        entry.success = (sqlite3_column_int(stmt, 5) == 1);
        const char* d = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        entry.details = d ? d : "";
        const char* ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        entry.client_ip = ip ? ip : "";
        list.push_back(entry);
    }
    sqlite3_finalize(stmt);
    return list;
}

int64_t MetadataManager::appendWal(const std::string& tx_id, const std::string& op, const std::string& file_path,
                                   const std::string& state, const std::string& payload) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "INSERT INTO wal_logs (timestamp, tx_id, operation, file_path, state, payload) "
                      "VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_text(stmt, 2, tx_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, op.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, file_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, payload.c_str(), -1, SQLITE_TRANSIENT);

    int64_t lsn = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        lsn = sqlite3_last_insert_rowid(db_);
    }
    sqlite3_finalize(stmt);
    return lsn;
}

bool MetadataManager::updateWalState(const std::string& tx_id, const std::string& new_state) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "UPDATE wal_logs SET state = ? WHERE tx_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, new_state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, tx_id.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<WalLogEntry> MetadataManager::getPendingWalEntries() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "SELECT lsn, timestamp, tx_id, operation, file_path, state, payload FROM wal_logs "
                      "WHERE state = 'PENDING' ORDER BY lsn ASC;";
    sqlite3_stmt* stmt = nullptr;
    std::vector<WalLogEntry> list;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WalLogEntry w;
        w.lsn = sqlite3_column_int64(stmt, 0);
        w.timestamp = sqlite3_column_int64(stmt, 1);
        w.tx_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        w.operation = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* fp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        w.file_path = fp ? fp : "";
        w.state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        w.payload = p ? p : "";
        list.push_back(w);
    }
    sqlite3_finalize(stmt);
    return list;
}

std::vector<WalLogEntry> MetadataManager::getRecentWalEntries(int limit) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "SELECT lsn, timestamp, tx_id, operation, file_path, state, payload FROM wal_logs "
                      "ORDER BY lsn DESC LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    std::vector<WalLogEntry> list;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WalLogEntry w;
        w.lsn = sqlite3_column_int64(stmt, 0);
        w.timestamp = sqlite3_column_int64(stmt, 1);
        w.tx_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        w.operation = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* fp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        w.file_path = fp ? fp : "";
        w.state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        w.payload = p ? p : "";
        list.push_back(w);
    }
    sqlite3_finalize(stmt);
    return list;
}

bool MetadataManager::verifyUser(const std::string& username, const std::string& password) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* sql = "SELECT role FROM users WHERE username = ? AND password_hash = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return ok;
}

bool MetadataManager::hasPermission(const std::string& username, const std::string& path, const std::string& op) {
    if (username == "admin") return true;

    auto f_opt = getFileByPath(path);
    if (!f_opt) {
        // If creating new file, users can write
        return (op == "WRITE" || op == "UPLOAD");
    }

    if (f_opt->owner == username) return true;

    // Check permissions bitmask
    // 0777: Owner=rwx (7), Group=rwx (7), Others=rwx (7)
    // 0755: Owner=rwx (7), Group=rx (5), Others=rx (5)
    int perm = f_opt->permissions;
    if (op == "READ" || op == "DOWNLOAD") {
        return (perm & 0004) != 0; // Other read
    }
    if (op == "WRITE" || op == "DELETE" || op == "RENAME" || op == "UPLOAD") {
        return (perm & 0002) != 0; // Other write
    }
    return false;
}

} // namespace dfs
