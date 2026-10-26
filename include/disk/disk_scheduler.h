#pragma once

#include "common/types.h"
#include <vector>
#include <string>

namespace dfs {

class DiskSchedulerSimulator {
public:
    static DiskSimResult runFcfs(int start_cylinder, const std::vector<int>& requests);
    static DiskSimResult runSstf(int start_cylinder, const std::vector<int>& requests);
    static DiskSimResult runScan(int start_cylinder, const std::vector<int>& requests, int max_cylinder = 199, bool moving_up = true);

    struct ComparisonSummary {
        DiskSimResult fcfs;
        DiskSimResult sstf;
        DiskSimResult scan;
        std::string best_algorithm;
        std::string disclaimer = "DISCLAIMER: This is an OS educational simulation. It demonstrates I/O scheduling principles and does not alter the physical Windows disk controller.";
    };

    static ComparisonSummary compareAll(int start_cylinder, const std::vector<int>& requests, int max_cylinder = 199);
};

} // namespace dfs
