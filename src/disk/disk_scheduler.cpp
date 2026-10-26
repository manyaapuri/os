#include "disk/disk_scheduler.h"
#include <cmath>
#include <algorithm>

namespace dfs {

DiskSimResult DiskSchedulerSimulator::runFcfs(int start_cylinder, const std::vector<int>& requests) {
    DiskSimResult res;
    res.algorithm = "FCFS";
    res.start_cylinder = start_cylinder;
    res.seek_sequence.push_back(start_cylinder);

    int current = start_cylinder;
    for (int req : requests) {
        res.total_head_movement += std::abs(req - current);
        current = req;
        res.seek_sequence.push_back(current);
    }

    if (!requests.empty()) {
        res.avg_seek_distance = static_cast<double>(res.total_head_movement) / requests.size();
    }
    return res;
}

DiskSimResult DiskSchedulerSimulator::runSstf(int start_cylinder, const std::vector<int>& requests) {
    DiskSimResult res;
    res.algorithm = "SSTF";
    res.start_cylinder = start_cylinder;
    res.seek_sequence.push_back(start_cylinder);

    std::vector<int> pending = requests;
    int current = start_cylinder;

    while (!pending.empty()) {
        auto best_it = pending.begin();
        int min_dist = std::abs(*best_it - current);

        for (auto it = pending.begin() + 1; it != pending.end(); ++it) {
            int dist = std::abs(*it - current);
            if (dist < min_dist) {
                min_dist = dist;
                best_it = it;
            }
        }

        res.total_head_movement += min_dist;
        current = *best_it;
        res.seek_sequence.push_back(current);
        pending.erase(best_it);
    }

    if (!requests.empty()) {
        res.avg_seek_distance = static_cast<double>(res.total_head_movement) / requests.size();
    }
    return res;
}

DiskSimResult DiskSchedulerSimulator::runScan(int start_cylinder, const std::vector<int>& requests, int max_cylinder, bool moving_up) {
    DiskSimResult res;
    res.algorithm = "SCAN";
    res.start_cylinder = start_cylinder;
    res.seek_sequence.push_back(start_cylinder);

    std::vector<int> left, right;
    for (int r : requests) {
        if (r < start_cylinder) left.push_back(r);
        else if (r > start_cylinder) right.push_back(r);
    }

    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());

    int current = start_cylinder;

    if (moving_up) {
        // Move towards max_cylinder first
        for (int r : right) {
            res.total_head_movement += std::abs(r - current);
            current = r;
            res.seek_sequence.push_back(current);
        }
        if (!left.empty()) {
            // Touch the outer edge then reverse
            res.total_head_movement += std::abs(max_cylinder - current);
            current = max_cylinder;
            res.seek_sequence.push_back(current);

            for (auto it = left.rbegin(); it != left.rend(); ++it) {
                res.total_head_movement += std::abs(*it - current);
                current = *it;
                res.seek_sequence.push_back(current);
            }
        }
    } else {
        // Move towards 0 first
        for (auto it = left.rbegin(); it != left.rend(); ++it) {
            res.total_head_movement += std::abs(*it - current);
            current = *it;
            res.seek_sequence.push_back(current);
        }
        if (!right.empty()) {
            res.total_head_movement += std::abs(0 - current);
            current = 0;
            res.seek_sequence.push_back(current);

            for (int r : right) {
                res.total_head_movement += std::abs(r - current);
                current = r;
                res.seek_sequence.push_back(current);
            }
        }
    }

    if (!requests.empty()) {
        res.avg_seek_distance = static_cast<double>(res.total_head_movement) / requests.size();
    }
    return res;
}

DiskSchedulerSimulator::ComparisonSummary DiskSchedulerSimulator::compareAll(int start_cylinder, const std::vector<int>& requests, int max_cylinder) {
    ComparisonSummary summary;
    summary.fcfs = runFcfs(start_cylinder, requests);
    summary.sstf = runSstf(start_cylinder, requests);
    summary.scan = runScan(start_cylinder, requests, max_cylinder);

    int min_mov = summary.fcfs.total_head_movement;
    summary.best_algorithm = "FCFS";

    if (summary.sstf.total_head_movement < min_mov) {
        min_mov = summary.sstf.total_head_movement;
        summary.best_algorithm = "SSTF";
    }
    if (summary.scan.total_head_movement < min_mov) {
        min_mov = summary.scan.total_head_movement;
        summary.best_algorithm = "SCAN";
    }

    return summary;
}

} // namespace dfs
