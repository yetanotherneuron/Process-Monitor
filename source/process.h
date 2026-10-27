#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace monitor {

struct ProcessEntry {
    std::uint32_t pid = 0;
    std::string name;
    std::uint64_t memoryBytes = 0;
    double cpuPercent = 0.0;
    std::uint64_t creationTime100Ns = 0;
    std::uint64_t userTime100Ns = 0;
    std::uint64_t kernelTime100Ns = 0;
    std::uint64_t totalCpuTime100Ns = 0;
    bool memoryAvailable = false;
    bool cpuTimeAvailable = false;
    bool cpuAvailable = false;
};

struct ProcessSnapshot {
    std::vector<ProcessEntry> processes;
    std::string error;
};

ProcessSnapshot listProcesses();
std::vector<ProcessEntry> filterProcesses(const std::vector<ProcessEntry>& processes, const std::string& filter);
std::vector<ProcessEntry> sortProcesses(std::vector<ProcessEntry> processes, const std::string& sortField);
std::string toCsv(const std::vector<ProcessEntry>& processes);

} // namespace monitor
