#include "process.h"

#include "utilities.h"

#include <algorithm>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

namespace {

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE get() const noexcept {
        return handle_;
    }

private:
    HANDLE handle_;
};

std::string toUtf8(const std::wstring& wideText) {
    if (wideText.empty()) {
        return {};
    }

    const int requiredLength = WideCharToMultiByte(CP_UTF8, 0, wideText.data(), static_cast<int>(wideText.size()), nullptr, 0, nullptr, nullptr);
    if (requiredLength <= 0) {
        return {};
    }

    std::string output(static_cast<std::size_t>(requiredLength), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wideText.data(), static_cast<int>(wideText.size()), output.data(), requiredLength, nullptr, nullptr) <= 0) {
        return {};
    }

    return output;
}

std::string errorMessage(DWORD errorCode) {
    wchar_t* messageBuffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&messageBuffer),
        0,
        nullptr);

    std::string message;
    if (length != 0 && messageBuffer != nullptr) {
        message = toUtf8(std::wstring(messageBuffer, length));
        LocalFree(messageBuffer);
    }

    if (message.empty()) {
        message = "Windows error " + std::to_string(errorCode);
    }

    return monitor::trim(message);
}

std::uint64_t filetimeTo100Ns(const FILETIME& timeValue) {
    return (static_cast<std::uint64_t>(timeValue.dwHighDateTime) << 32U) |
           static_cast<std::uint64_t>(timeValue.dwLowDateTime);
}

} // namespace

namespace monitor {

ProcessSnapshot listProcesses() {
    ProcessSnapshot snapshot;

    UniqueHandle processSnapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (processSnapshot.get() == INVALID_HANDLE_VALUE) {
        snapshot.error = "Unable to enumerate processes: " + errorMessage(GetLastError());
        return snapshot;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(processSnapshot.get(), &entry)) {
        const DWORD errorCode = GetLastError();
        if (errorCode != ERROR_NO_MORE_FILES) {
            snapshot.error = "Unable to read process list: " + errorMessage(errorCode);
        }
        return snapshot;
    }

    do {
        ProcessEntry process;
        process.pid = entry.th32ProcessID;
        process.name = toUtf8(entry.szExeFile);

        UniqueHandle queryHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.pid));
        if (queryHandle.get() != nullptr) {
            FILETIME creationTime{};
            FILETIME exitTime{};
            FILETIME kernelTime{};
            FILETIME userTime{};

            if (GetProcessTimes(queryHandle.get(), &creationTime, &exitTime, &kernelTime, &userTime)) {
                process.creationTime100Ns = filetimeTo100Ns(creationTime);
                process.kernelTime100Ns = filetimeTo100Ns(kernelTime);
                process.userTime100Ns = filetimeTo100Ns(userTime);
                process.totalCpuTime100Ns = process.kernelTime100Ns + process.userTime100Ns;
                process.cpuTimeAvailable = true;
            }
        }

        UniqueHandle memoryHandle(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, process.pid));
        if (memoryHandle.get() != nullptr) {
            PROCESS_MEMORY_COUNTERS_EX counters{};
            counters.cb = sizeof(counters);
            if (GetProcessMemoryInfo(
                    memoryHandle.get(),
                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                    sizeof(counters))) {
                process.memoryBytes = static_cast<std::uint64_t>(counters.PrivateUsage);
                process.memoryAvailable = true;
            }
        }

        snapshot.processes.push_back(std::move(process));
    } while (Process32NextW(processSnapshot.get(), &entry));

    const DWORD enumerationError = GetLastError();
    if (enumerationError != ERROR_NO_MORE_FILES) {
        snapshot.error = "Process list may be incomplete: " + errorMessage(enumerationError);
    }

    return snapshot;
}

std::vector<ProcessEntry> filterProcesses(const std::vector<ProcessEntry>& processes, const std::string& filter) {
    std::vector<ProcessEntry> filtered;
    filtered.reserve(processes.size());

    for (const auto& process : processes) {
        const std::string pidText = std::to_string(process.pid);
        if (matchesFilter(process.name, filter) || matchesFilter(pidText, filter)) {
            filtered.push_back(process);
        }
    }

    return filtered;
}

std::vector<ProcessEntry> sortProcesses(std::vector<ProcessEntry> processes, const std::string& sortField) {
    const std::string normalizedSort = toLower(trim(sortField));

    if (normalizedSort == "pid") {
        std::sort(processes.begin(), processes.end(), [](const ProcessEntry& left, const ProcessEntry& right) {
            return left.pid < right.pid;
        });
    } else if (normalizedSort == "name") {
        std::sort(processes.begin(), processes.end(), [](const ProcessEntry& left, const ProcessEntry& right) {
            return toLower(left.name) < toLower(right.name);
        });
    } else if (normalizedSort == "cpu") {
        std::sort(processes.begin(), processes.end(), [](const ProcessEntry& left, const ProcessEntry& right) {
            if (left.cpuAvailable != right.cpuAvailable) {
                return left.cpuAvailable;
            }
            if (left.cpuPercent != right.cpuPercent) {
                return left.cpuPercent > right.cpuPercent;
            }
            return left.pid < right.pid;
        });
    } else {
        std::sort(processes.begin(), processes.end(), [](const ProcessEntry& left, const ProcessEntry& right) {
            if (left.memoryAvailable != right.memoryAvailable) {
                return left.memoryAvailable;
            }
            if (left.memoryBytes != right.memoryBytes) {
                return left.memoryBytes > right.memoryBytes;
            }
            return left.pid < right.pid;
        });
    }

    return processes;
}

std::string toCsv(const std::vector<ProcessEntry>& processes) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "process_name,pid,private_bytes,cpu_percent\r\n";

    for (const auto& process : processes) {
        output << escapeCsvField(process.name) << ',' << process.pid << ',';
        if (process.memoryAvailable) {
            output << process.memoryBytes;
        }
        output << ',';
        if (process.cpuAvailable) {
            output << std::fixed << std::setprecision(2) << process.cpuPercent;
        }
        output << "\r\n";
    }

    return output.str();
}

} // namespace monitor
