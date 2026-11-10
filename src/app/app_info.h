#pragma once
#include "model.h"

namespace contour::app {
Json application_info(std::string_view webview_version);
Json license_notices(const std::filesystem::path& assets);
std::wstring information_link(std::string_view name);
void open_information_link(HWND owner,std::string_view name);
void copy_information(HWND owner,std::string_view text);
}
