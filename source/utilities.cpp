#include "utilities.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace monitor {

std::string trim(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string formatMemory(std::uint64_t bytes) {
    static constexpr std::array<const char*, 6> units = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = static_cast<double>(bytes);
    std::size_t index = 0;

    while (value >= 1024.0 && index + 1 < units.size()) {
        value /= 1024.0;
        ++index;
    }

    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value << " " << units[index];
    return stream.str();
}

std::string escapeCsvField(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) {
        return value;
    }

    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');

    for (char ch : value) {
        if (ch == '"') {
            escaped += "\"\"";
        } else {
            escaped.push_back(ch);
        }
    }

    escaped.push_back('"');
    return escaped;
}

bool matchesFilter(const std::string& candidate, const std::string& filter) {
    const std::string normalizedCandidate = toLower(trim(candidate));
    const std::string normalizedFilter = toLower(trim(filter));

    if (normalizedFilter.empty()) {
        return true;
    }

    return normalizedCandidate.find(normalizedFilter) != std::string::npos;
}

} // namespace monitor
