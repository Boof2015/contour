#include <contour/profile.h>
#include <windows.h>
#include <objbase.h>
#include <charconv>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace contour::eq {
namespace {
constexpr std::size_t max_bytes = 1024*1024;
struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~File() { if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); } }
};
void check(bool condition, const char* message) { if (!condition) { throw std::invalid_argument(message); } }
void complete(std::istringstream& line) {
    check(!line.fail(), "Invalid or missing field");
    line >> std::ws;
    check(line.eof(), "Unexpected trailing fields");
}
bool boolean(unsigned number) { check(number <= 1, "Boolean must be 0 or 1"); return number != 0; }
unsigned unsigned_field(std::istringstream& line) {
    std::string token; line >> token;
    unsigned value = 0;
    const auto parsed = std::from_chars(token.data(), token.data()+token.size(), value);
    check(parsed.ec == std::errc{} && parsed.ptr == token.data()+token.size(), "Invalid unsigned field");
    return value;
}
bool transient_file_error(DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_ACCESS_DENIED;
}
} // namespace

void validate_profile(const Profile& profile) {
    for (const auto& layer : profile.configuration.layers) {
        check(layer.name.size() <= 1024 && layer.name.find_first_of("\r\n\0", 0, 3) == std::string::npos,
              "Layer name must be at most 1024 bytes without newline/NUL");
    }
    ParametricEq validation;
    validation.configure(profile.configuration, profile.validation_sample_rate, profile.channels);
}

std::string serialize_profile(const Profile& profile) {
    validate_profile(profile);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << "CONTOUR 1 " << profile.validation_sample_rate << ' ' << profile.channels << '\n';
    out << "Bypass " << profile.configuration.bypass << '\n';
    for (const auto& layer : profile.configuration.layers) {
        out << "Layer " << std::quoted(layer.name) << ' ' << layer.enabled << ' ' << layer.channels.all
            << ' ' << layer.channels.mask << ' ' << layer.gain_db << '\n';
        for (const auto& f : layer.filters) {
            out << "Filter " << static_cast<unsigned>(f.type) << ' ' << f.enabled << ' ' << f.frequency_hz
                << ' ' << f.gain_db << ' ' << f.q << '\n';
        }
        out << "EndLayer\n";
    }
    out << "End\n";
    check(out.str().size() <= max_bytes, "Profile exceeds 1 MiB");
    return out.str();
}

Profile parse_profile(std::string_view text) {
    check(text.size() <= max_bytes && text.find('\0') == text.npos, "Profile is oversized or contains NUL");
    if (text.starts_with("\xEF\xBB\xBF")) { text.remove_prefix(3); }
    Profile profile;
    bool header = false, bypass = false, ended = false, in_layer = false;
    std::size_t number = 0;
    while (!text.empty()) {
        ++number;
        const auto newline = text.find('\n');
        const std::string row(text.substr(0, newline));
        text.remove_prefix(newline == text.npos ? text.size() : newline+1);
        try {
            check(row.size() <= 8192, "Profile line exceeds 8192 bytes");
            std::istringstream line(row);
            line.imbue(std::locale::classic());
            std::string command;
            if (!(line >> command)) { continue; }
            check(!ended, "Content after End");
            if (!header) {
                const auto version = unsigned_field(line);
                line >> profile.validation_sample_rate;
                profile.channels = unsigned_field(line);
                complete(line);
                check(command == "CONTOUR" && version == 1, "Unsupported profile header/version");
                header = true;
            } else if (!bypass) {
                const auto value = unsigned_field(line); complete(line);
                check(command == "Bypass", "Expected Bypass");
                profile.configuration.bypass = boolean(value); bypass = true;
            } else if (command == "Layer") {
                check(!in_layer && profile.configuration.layers.size() < max_layers, "Nested or excessive layers");
                Layer layer;
                line >> std::ws;
                check(line.peek() == '"', "Layer names must be quoted");
                line >> std::quoted(layer.name);
                const auto enabled = unsigned_field(line), all = unsigned_field(line);
                layer.channels.mask = unsigned_field(line);
                line >> layer.gain_db;
                complete(line);
                layer.enabled = boolean(enabled); layer.channels.all = boolean(all);
                profile.configuration.layers.push_back(std::move(layer)); in_layer = true;
            } else if (command == "Filter") {
                check(in_layer && profile.configuration.layers.back().filters.size() < max_filters_per_layer,
                      "Filter outside layer or excessive filters");
                Filter filter;
                const auto type = unsigned_field(line), enabled = unsigned_field(line);
                line >> filter.frequency_hz >> filter.gain_db >> filter.q; complete(line);
                check(type <= static_cast<unsigned>(FilterType::band_pass), "Unknown filter type");
                filter.type = static_cast<FilterType>(type); filter.enabled = boolean(enabled);
                profile.configuration.layers.back().filters.push_back(filter);
            } else if (command == "EndLayer") {
                complete(line); check(in_layer, "EndLayer without Layer"); in_layer = false;
            } else if (command == "End") {
                complete(line); check(!in_layer, "Unclosed layer"); ended = true;
            } else { throw std::invalid_argument("Unknown profile field"); }
        } catch (const std::invalid_argument& error) {
            throw std::invalid_argument("Profile line " + std::to_string(number) + ": " + error.what());
        }
    }
    check(header && bypass && ended && !in_layer, "Incomplete profile");
    validate_profile(profile);
    return profile;
}

std::optional<std::string> read_profile_file(const std::filesystem::path& path) {
    File file;
    DWORD error = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 20; ++attempt) {
        file.handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file.handle != INVALID_HANDLE_VALUE) { break; }
        error = GetLastError();
        if (!transient_file_error(error)) { break; }
        Sleep(5); // Control thread only; pending rename/deletion can be transient.
    }
    if (file.handle == INVALID_HANDLE_VALUE) {
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { return {}; }
        throw std::runtime_error("Cannot read profile; Windows error " + std::to_string(error));
    }
    LARGE_INTEGER size{};
    check(GetFileSizeEx(file.handle, &size) && size.QuadPart >= 0 && size.QuadPart <= max_bytes, "Invalid profile file size");
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD received = 0;
    check(ReadFile(file.handle, text.data(), static_cast<DWORD>(text.size()), &received, nullptr) &&
          received == text.size(), "Could not read complete profile");
    return text;
}

void save_profile_file(const std::filesystem::path& path, const Profile& profile) {
    const auto text = serialize_profile(profile);
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) { throw std::runtime_error("Cannot generate profile transaction ID"); }
    wchar_t suffix[40]{};
    StringFromGUID2(id, suffix, 40);
    const auto temp = std::filesystem::path(path.wstring() + L"." + suffix + L".tmp");
    try {
        {
            File file{CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
            if (file.handle == INVALID_HANDLE_VALUE) { throw std::runtime_error("Cannot create profile transaction; Windows error " + std::to_string(GetLastError())); }
            DWORD written = 0;
            check(WriteFile(file.handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                  written == text.size() && FlushFileBuffers(file.handle), "Cannot persist complete profile");
        }
        DWORD error = ERROR_SUCCESS;
        bool published = false;
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) { published = true; break; }
            error = GetLastError();
            if (!transient_file_error(error)) { break; }
            Sleep(5);
        }
        if (!published) { throw std::runtime_error("Cannot publish profile; Windows error " + std::to_string(error)); }
    } catch (...) { DeleteFileW(temp.c_str()); throw; }
}
} // namespace contour::eq
