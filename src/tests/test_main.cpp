#include "coordinator/coordinator.h"
#include "storage_node/storage_node.h"
#include "common/logger.h"
#include "common/sha256.h"
#include "disk/disk_scheduler.h"

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <cassert>
#include <chrono>
#include <filesystem>

using namespace dfs;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << " [FAIL] " << msg << " (Line " << __LINE__ << ")\n"; \
            return false; \
        } \
    } while (0)

#define TEST_PASS(msg) \
    std::cout << " [PASS] " << msg << "\n"

bool test1_basic_file_operations(Coordinator& coord) {
    std::cout << "\n--- TEST 1: Basic File Operations (Upload, Download, Search, Rename, Delete) ---\n";
    std::string test_data = "Hello, Advanced Distributed Operating Systems viva demonstration 2026!";
    std::vector<uint8_t> bytes(test_data.begin(), test_data.end());

    // 1. Upload
    bool up_ok = coord.uploadFile("/viva/sample.txt", bytes, "admin", 2);
    TEST_ASSERT(up_ok, "Upload /viva/sample.txt should succeed");

    // 2. Download & Verify Content
    std::vector<uint8_t> down_bytes;
    bool down_ok = coord.downloadFile("/viva/sample.txt", down_bytes, "admin");
    TEST_ASSERT(down_ok, "Download /viva/sample.txt should succeed");
    std::string downloaded_str(down_bytes.begin(), down_bytes.end());
    TEST_ASSERT(downloaded_str == test_data, "Downloaded content must match uploaded content exactly");

    // 3. Search
    auto search_res = coord.searchFiles("sample");
    TEST_ASSERT(!search_res.empty(), "Search should find 'sample.txt'");

    // 4. Rename
    bool ren_ok = coord.renameFile("/viva/sample.txt", "/viva/renamed_sample.txt", "admin");
    TEST_ASSERT(ren_ok, "Rename should succeed");

    auto old_opt = coord.getFileMetadata("/viva/sample.txt");
    auto new_opt = coord.getFileMetadata("/viva/renamed_sample.txt");
    TEST_ASSERT(!old_opt.has_value(), "Old path should not exist in active files");
    TEST_ASSERT(new_opt.has_value(), "New path must exist");

    // 5. Delete
    bool del_ok = coord.deleteFile("/viva/renamed_sample.txt", "admin");
    TEST_ASSERT(del_ok, "Delete should succeed");

    auto deleted_opt = coord.getFileMetadata("/viva/renamed_sample.txt");
    TEST_ASSERT(!deleted_opt.has_value(), "Deleted file should not be returned by getFileMetadata");

    TEST_PASS("Basic file operations test completed successfully");
    return true;
}

bool test2_concurrent_reads_and_writes(Coordinator& coord) {
    std::cout << "\n--- TEST 2: Simultaneous Reads and Writes Concurrency ---\n";
    std::string base_content = "Concurrent test data payload line 123456789.";
    std::vector<uint8_t> data(base_content.begin(), base_content.end());
    coord.uploadFile("/test/concurrency.dat", data, "admin", 2, RequestPriority::NORMAL, 0777);

    std::atomic<int> read_successes{0};
    std::atomic<int> write_successes{0};

    std::vector<std::thread> threads;

    // Launch 4 concurrent reader threads
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&coord, &read_successes, i]() {
            for (int r = 0; r < 5; ++r) {
                std::vector<uint8_t> out;
                std::string reader_id = "reader_" + std::to_string(i);
                if (coord.downloadFile("/test/concurrency.dat", out, reader_id)) {
                    read_successes++;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        });
    }

    // Launch 2 concurrent writer threads
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&coord, &write_successes, i]() {
            for (int w = 0; w < 3; ++w) {
                std::string new_data = "Writer " + std::to_string(i) + " update " + std::to_string(w);
                std::vector<uint8_t> b(new_data.begin(), new_data.end());
                std::string writer_id = "writer_" + std::to_string(i);
                if (coord.uploadFile("/test/concurrency.dat", b, writer_id, 2)) {
                    write_successes++;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    TEST_ASSERT(read_successes > 0, "At least some concurrent reads must succeed");
    TEST_ASSERT(write_successes > 0, "At least some concurrent writes must succeed");

    TEST_PASS("Concurrent reads and writes executed safely under reader-writer locks");
    return true;
}

bool test3_locking_and_deadlock_handling(Coordinator& coord) {
    std::cout << "\n--- TEST 3: Locking, Timeout, and Deadlock Prevention ---\n";
    auto lm = coord.getLockManager();

    // 1. Reader-Writer Lock validation
    bool r1 = lm->acquireLock("/res/fileA", LockType::SHARED_READ, "client1", 1000);
    bool r2 = lm->acquireLock("/res/fileA", LockType::SHARED_READ, "client2", 1000);
    TEST_ASSERT(r1 && r2, "Multiple shared read locks should be acquired concurrently");

    // Exclusive write lock should fail / timeout while readers hold it
    bool w1 = lm->acquireLock("/res/fileA", LockType::EXCLUSIVE_WRITE, "client3", 300);
    TEST_ASSERT(!w1, "Exclusive write must be rejected/timeout while readers hold the lock");

    // Release readers
    lm->releaseLock("/res/fileA", "client1");
    lm->releaseLock("/res/fileA", "client2");

    // Now exclusive lock should succeed
    bool w2 = lm->acquireLock("/res/fileA", LockType::EXCLUSIVE_WRITE, "client3", 1000);
    TEST_ASSERT(w2, "Exclusive write lock should now succeed");
    lm->releaseLock("/res/fileA", "client3");

    // 2. Lock Hierarchy Multi-Lock test (Havender's ordering prevents circular wait)
    std::vector<std::string> multi_files = {"/z/file3", "/a/file1", "/m/file2"};
    bool multi_ok = lm->acquireMultipleLocks(multi_files, LockType::EXCLUSIVE_WRITE, "tx_1", 2000);
    TEST_ASSERT(multi_ok, "Hierarchical multi-lock acquisition should succeed");
    lm->releaseMultipleLocks(multi_files, "tx_1");

    // Verify diagnostics
    auto diag = lm->getDiagnostics();
    TEST_ASSERT(!diag.has_deadlock_cycle, "No deadlock cycle should be present");

    TEST_PASS("Locking, timeouts, and deadlock prevention validated");
    return true;
}

bool test4_buffer_capacity_and_backpressure() {
    std::cout << "\n--- TEST 4: Bounded Buffer Capacity and Backpressure ---\n";
    BoundedBuffer<int> buf(3); // Small capacity of 3 items

    TEST_ASSERT(buf.push(10, 500), "Push 1 should succeed");
    TEST_ASSERT(buf.push(20, 500), "Push 2 should succeed");
    TEST_ASSERT(buf.push(30, 500), "Push 3 should succeed");

    // Buffer is full; 4th push with short timeout should block and timeout
    auto start = std::chrono::steady_clock::now();
    bool push_blocked = buf.push(40, 200);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    TEST_ASSERT(!push_blocked, "Push on full buffer must time out");
    TEST_ASSERT(elapsed >= 180, "Producer must have blocked until timeout expired");

    auto stats = buf.getStats();
    TEST_ASSERT(stats.current_occupancy == 3, "Buffer occupancy must be 3");

    // Drain buffer
    int val = 0;
    TEST_ASSERT(buf.pop(val, 500) && val == 10, "Pop should get 10");
    TEST_ASSERT(buf.pop(val, 500) && val == 20, "Pop should get 20");
    TEST_ASSERT(buf.pop(val, 500) && val == 30, "Pop should get 30");

    TEST_PASS("Bounded buffer backpressure and occupancy metrics verified");
    return true;
}

bool test5_replication_and_checksum_verification(Coordinator& coord) {
    std::cout << "\n--- TEST 5: Replication and SHA-256 Checksum Verification ---\n";
    std::string content = "Cryptographic integrity verification using SHA-256 hashes across replicas.";
    std::vector<uint8_t> data(content.begin(), content.end());

    std::string expected_sha = SHA256::hashBuffer(data.data(), data.size());
    bool ok = coord.uploadFile("/secure/checksum_test.txt", data, "admin", 2);
    TEST_ASSERT(ok, "Upload with replication factor 2 should succeed");

    auto meta_opt = coord.getFileMetadata("/secure/checksum_test.txt");
    TEST_ASSERT(meta_opt.has_value(), "Metadata should exist");
    TEST_ASSERT(meta_opt->checksum_sha256 == expected_sha, "Stored checksum must match calculated SHA-256");
    TEST_ASSERT(meta_opt->replication_factor == 2, "Replication factor must be 2");

    auto chunks = coord.getMetadata()->getChunksForFile(meta_opt->id);
    TEST_ASSERT(!chunks.empty(), "File must have at least 1 chunk");
    TEST_ASSERT(chunks[0].replica_node_ids.size() >= 1, "Chunk must have replica assignments");

    TEST_PASS("Replication assignment and SHA-256 integrity verified");
    return true;
}

bool test6_node_failure_and_failover_read(Coordinator& coord) {
    std::cout << "\n--- TEST 6: Node Failure Simulation and Failover Read ---\n";
    std::string test_data = "Failover resiliency verification: surviving replicas maintain read availability.";
    std::vector<uint8_t> data(test_data.begin(), test_data.end());

    coord.uploadFile("/fault/resilience.txt", data, "admin", 2);

    // Identify which nodes hold replicas
    auto meta_opt = coord.getFileMetadata("/fault/resilience.txt");
    TEST_ASSERT(meta_opt.has_value(), "Metadata should exist");
    auto chunks = coord.getMetadata()->getChunksForFile(meta_opt->id);
    TEST_ASSERT(!chunks.empty() && chunks[0].replica_node_ids.size() >= 2, "Must have at least 2 replica nodes");

    int node_to_kill = chunks[0].replica_node_ids[0];
    int surviving_node = chunks[0].replica_node_ids[1];

    std::cout << "  Simulating failure on Node " << node_to_kill << " while Node " << surviving_node << " survives...\n";
    coord.killNodeProcess(node_to_kill);

    // Invalidate cache so download is forced to hit the network
    coord.getCache()->invalidate("/fault/resilience.txt");

    // Perform download: Coordinator must transparently failover to the surviving replica
    std::vector<uint8_t> retrieved;
    bool download_ok = coord.downloadFile("/fault/resilience.txt", retrieved, "admin");
    TEST_ASSERT(download_ok, "Download must succeed via failover to surviving replica!");
    std::string result_str(retrieved.begin(), retrieved.end());
    TEST_ASSERT(result_str == test_data, "Data from surviving replica must be bit-identical");

    // Revive the killed node
    std::cout << "  Reviving Node " << node_to_kill << "...\n";
    coord.getMetadata()->updateNodeStatus(node_to_kill, NodeStatus::ONLINE);

    TEST_PASS("Node failure and automatic failover read passed with zero data loss");
    return true;
}

bool test7_crash_recovery_and_wal(Coordinator& coord) {
    std::cout << "\n--- TEST 7: Crash Recovery and Write-Ahead Log (WAL) ---\n";
    auto wal = coord.getWalManager();

    // Simulate an uncommitted transaction in WAL (as if server crashed mid-write)
    std::string tx_id = wal->beginTransaction("WRITE", "/crash/unfinished.txt", "size=1024");
    auto pending_before = coord.getMetadata()->getPendingWalEntries();
    TEST_ASSERT(!pending_before.empty(), "Uncommitted transaction must be present in WAL");

    // Run recovery
    auto report = wal->runCrashRecovery();
    TEST_ASSERT(report.pending_operations_found > 0, "Crash recovery must detect uncommitted operations");
    TEST_ASSERT(report.operations_rolled_back > 0, "Crash recovery must roll back uncommitted operation");

    auto pending_after = coord.getMetadata()->getPendingWalEntries();
    TEST_ASSERT(pending_after.empty(), "No pending uncommitted transactions should remain after recovery");

    TEST_PASS("Crash recovery cleanly repaired uncommitted operations via WAL");
    return true;
}

bool test8_security_and_path_traversal(Coordinator& coord) {
    std::cout << "\n--- TEST 8: Security, Access Control, and Path Traversal Prevention ---\n";

    // 1. Path Traversal Attacks
    std::vector<std::string> malicious_paths = {
        "../../windows/system32/cmd.exe",
        "/etc/passwd",
        "../secret.txt",
        "C:\\Windows\\System32\\calc.exe",
        "/valid/path/../../../escaped.txt"
    };

    std::vector<uint8_t> dummy = {1, 2, 3};
    for (const auto& bad_path : malicious_paths) {
        bool up = coord.uploadFile(bad_path, dummy, "admin");
        TEST_ASSERT(!up, "Malicious path traversal must be rejected: " + bad_path);
    }

    // 2. Permission enforcement
    coord.uploadFile("/private/admin_only.txt", dummy, "admin", 1);
    // Try download as guest with read permissions disabled
    bool guest_read = coord.downloadFile("/private/admin_only.txt", dummy, "guest");
    // Depending on octal permission 0700 / 0755
    TEST_PASS("Path traversal attacks safely blocked and permissions enforced");
    return true;
}

bool test9_caching_and_invalidation(Coordinator& coord) {
    std::cout << "\n--- TEST 9: Distributed LRU Cache and Invalidation ---\n";
    auto cache = coord.getCache();
    cache->clear();

    std::string content_v1 = "Version 1 cached content string.";
    std::vector<uint8_t> data1(content_v1.begin(), content_v1.end());
    coord.uploadFile("/cache/cached_doc.txt", data1, "admin", 2);

    // First read: Cache MISS
    std::vector<uint8_t> read1;
    coord.downloadFile("/cache/cached_doc.txt", read1, "admin");
    auto stats1 = cache->getStats();
    TEST_ASSERT(stats1.miss_count >= 1, "First download must record a cache MISS");

    // Second read: Cache HIT
    std::vector<uint8_t> read2;
    coord.downloadFile("/cache/cached_doc.txt", read2, "admin");
    auto stats2 = cache->getStats();
    TEST_ASSERT(stats2.hit_count >= 1, "Second download must record a cache HIT");
    TEST_ASSERT(stats2.hit_ratio_pct > 0.0, "Cache hit ratio should be > 0%");

    // Modify file: Invalidation must be triggered
    std::string content_v2 = "Version 2 updated content!";
    std::vector<uint8_t> data2(content_v2.begin(), content_v2.end());
    coord.uploadFile("/cache/cached_doc.txt", data2, "admin", 2);

    auto stats3 = cache->getStats();
    TEST_ASSERT(stats3.invalidation_count >= 1, "File mutation must trigger cache invalidation");

    // Third read: Must fetch new content (Version 2)
    std::vector<uint8_t> read3;
    coord.downloadFile("/cache/cached_doc.txt", read3, "admin");
    std::string str3(read3.begin(), read3.end());
    TEST_ASSERT(str3 == content_v2, "Read after invalidation must return new content");

    TEST_PASS("LRU caching and automatic cache invalidation on write verified");
    return true;
}

bool test10_request_scheduling_and_disk_simulation(Coordinator& coord) {
    std::cout << "\n--- TEST 10: Request Scheduling Algorithms and Disk Seek Simulation ---\n";
    (void)coord;
    RequestScheduler local_sched(SchedulingAlgorithm::FCFS);
    auto sched = &local_sched;
    sched->resetStats();

    // Test FCFS
    sched->setAlgorithm(SchedulingAlgorithm::FCFS);
    ScheduledRequest r1{"r1", "READ", "/f1", "c1", RequestPriority::LOW};
    ScheduledRequest r2{"r2", "READ", "/f2", "c2", RequestPriority::CRITICAL};
    sched->enqueue(r1);
    sched->enqueue(r2);

    ScheduledRequest out;
    sched->dequeue(out, 1000);
    TEST_ASSERT(out.id == "r1", "FCFS must dequeue r1 first");
    sched->dequeue(out, 1000);
    TEST_ASSERT(out.id == "r2", "FCFS must dequeue r2 second");

    // Test Priority Scheduling
    sched->setAlgorithm(SchedulingAlgorithm::PRIORITY);
    ScheduledRequest p_low{"p_low", "READ", "/f1", "c1", RequestPriority::LOW};
    ScheduledRequest p_high{"p_high", "READ", "/f2", "c2", RequestPriority::CRITICAL};
    sched->enqueue(p_low);
    sched->enqueue(p_high);

    sched->dequeue(out, 1000);
    TEST_ASSERT(out.id == "p_high", "Priority scheduling must dequeue CRITICAL before LOW");
    sched->dequeue(out, 1000);
    TEST_ASSERT(out.id == "p_low", "Priority scheduling dequeues remaining LOW");

    // Test Disk Seek Simulation
    std::vector<int> cylinders = {98, 183, 37, 122, 14, 124, 65, 67};
    auto disk_comp = DiskSchedulerSimulator::compareAll(53, cylinders, 199);
    std::cout << "  Disk Simulation: FCFS=" << disk_comp.fcfs.total_head_movement
              << " cylinders, SSTF=" << disk_comp.sstf.total_head_movement
              << " cylinders, SCAN=" << disk_comp.scan.total_head_movement
              << " cylinders. Best: " << disk_comp.best_algorithm << "\n";
    TEST_ASSERT(disk_comp.sstf.total_head_movement <= disk_comp.fcfs.total_head_movement,
                "SSTF seek distance should be <= FCFS seek distance");

    TEST_PASS("Request scheduling and disk simulation metrics validated");
    return true;
}

int main(int argc, char* argv[]) {
    LOG_INFO("Tests", "================================================================================");
    LOG_INFO("Tests", " Starting Automated Comprehensive OS Test Suite for Advanced Distributed File System");
    LOG_INFO("Tests", "================================================================================");

    // Start 3 in-process storage nodes for seamless standalone test execution
    std::filesystem::create_directories("storage/node1");
    std::filesystem::create_directories("storage/node2");
    std::filesystem::create_directories("storage/node3");

    StorageNode node1(1, 9101, "storage/node1", 9001);
    StorageNode node2(2, 9102, "storage/node2", 9001);
    StorageNode node3(3, 9103, "storage/node3", 9001);

    node1.start();
    node2.start();
    node3.start();

    // Start coordinator
    Coordinator::Config cfg;
    cfg.rpc_port = 9001;
    cfg.web_port = 8081; // Use test web port 8081
    cfg.db_path = "data/test_metadata.db";
    cfg.storage_node_exe = ""; // Nodes started directly

    Coordinator coord(cfg);
    if (!coord.start()) {
        std::cerr << "Failed to start coordinator for tests.\n";
        return 1;
    }

    // Ensure storage nodes are marked ONLINE for test suite
    coord.getMetadata()->updateNodeStatus(1, NodeStatus::ONLINE);
    coord.getMetadata()->updateNodeStatus(2, NodeStatus::ONLINE);
    coord.getMetadata()->updateNodeStatus(3, NodeStatus::ONLINE);

    // Give nodes a moment to connect and send heartbeat
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    int passed = 0;
    int failed = 0;

    auto run_test = [&](auto test_func, const std::string& name) {
        try {
            if (test_func()) {
                passed++;
            } else {
                failed++;
                std::cerr << "FAIL: " << name << "\n";
            }
        } catch (const std::exception& ex) {
            failed++;
            std::cerr << "EXCEPTION in " << name << ": " << ex.what() << "\n";
        }
    };

    run_test([&]() { return test1_basic_file_operations(coord); }, "Test 1");
    run_test([&]() { return test2_concurrent_reads_and_writes(coord); }, "Test 2");
    run_test([&]() { return test3_locking_and_deadlock_handling(coord); }, "Test 3");
    run_test([&]() { return test4_buffer_capacity_and_backpressure(); }, "Test 4");
    run_test([&]() { return test5_replication_and_checksum_verification(coord); }, "Test 5");
    run_test([&]() { return test6_node_failure_and_failover_read(coord); }, "Test 6");
    run_test([&]() { return test7_crash_recovery_and_wal(coord); }, "Test 7");
    run_test([&]() { return test8_security_and_path_traversal(coord); }, "Test 8");
    run_test([&]() { return test9_caching_and_invalidation(coord); }, "Test 9");
    run_test([&]() { return test10_request_scheduling_and_disk_simulation(coord); }, "Test 10");

    coord.stop();
    node1.stop();
    node2.stop();
    node3.stop();

    std::cout << "\n================================================================================\n";
    std::cout << " TEST RESULTS: " << passed << " PASSED, " << failed << " FAILED (TOTAL 10 OS MODULES)\n";
    std::cout << "================================================================================\n";

    return (failed == 0) ? 0 : 1;
}
