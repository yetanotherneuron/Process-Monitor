#pragma once

#include <cstdint>
#include <string>

namespace monitor {

std::string formatMemory(std::uint64_t bytes);
std::string escapeCsvField(const std::string& value);
std::string trim(const std::string& value);
std::string toLower(std::string value);
bool matchesFilter(const std::string& candidate, const std::string& filter);

} // namespace monitor
