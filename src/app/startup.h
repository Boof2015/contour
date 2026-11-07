#pragma once
#include <windows.h>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace contour::app {
struct LaunchOptions {
    bool startup=false;
    std::optional<std::filesystem::path> data;
};
// Arguments exclude argv[0]. --startup never activates an existing instance.
LaunchOptions parse_launch_options(std::span<const std::wstring_view> args);

class StartupRegistration {
public:
    // The alternative root is only a native test seam, never a bridge argument.
    explicit StartupRegistration(std::filesystem::path executable,HKEY root=HKEY_CURRENT_USER);
    bool enabled() const;
    bool registered() const;
    void set_enabled(bool enabled);
private:
    std::optional<std::wstring> read() const;
    HKEY root_;
    std::wstring command_;
};
} // namespace contour::app
