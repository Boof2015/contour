#pragma once
#include <contour/peq.h>
#include <filesystem>
#include <optional>
#include <string_view>

namespace contour::eq {
struct Profile {
    Configuration configuration;
    double validation_sample_rate = 48000;
    std::size_t channels = 2;
};

// Versioned UTF-8 data, with no external references or executable commands.
// Parsing/validation/file operations are strictly non-real-time.
std::string serialize_profile(const Profile& profile);
Profile parse_profile(std::string_view text);
void validate_profile(const Profile& profile);
std::optional<std::string> read_profile_file(const std::filesystem::path& path);
void save_profile_file(const std::filesystem::path& path, const Profile& profile);
} // namespace contour::eq
