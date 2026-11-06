#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace contour::eq::detail {
// Non-real-time bounded reads and same-directory, flushed atomic replacement.
std::optional<std::string> read_file(const std::filesystem::path& path, std::size_t limit);
void write_file(const std::filesystem::path& path, std::string_view bytes);
}
