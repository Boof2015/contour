#include "file_storage.h"
#include <windows.h>
#include <objbase.h>
#include <limits>
#include <stdexcept>

namespace contour::eq::detail {
namespace {
struct File { HANDLE handle=INVALID_HANDLE_VALUE; ~File() { if (handle!=INVALID_HANDLE_VALUE) { CloseHandle(handle); } } };
bool transient(DWORD error) { return error==ERROR_SHARING_VIOLATION || error==ERROR_LOCK_VIOLATION || error==ERROR_ACCESS_DENIED; }
void require(bool condition,const char* message) { if (!condition) { throw std::runtime_error(message); } }
}
std::optional<std::string> read_file(const std::filesystem::path& path,std::size_t limit) {
    File file; DWORD error=ERROR_SUCCESS;
    for (int attempt=0; attempt<20; ++attempt) {
        file.handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file.handle!=INVALID_HANDLE_VALUE) { break; }
        error=GetLastError(); if (!transient(error)) { break; } Sleep(5);
    }
    if (file.handle==INVALID_HANDLE_VALUE) {
        if (error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND) { return {}; }
        throw std::runtime_error("Cannot read file; Windows error "+std::to_string(error));
    }
    LARGE_INTEGER size{};
    require(GetFileSizeEx(file.handle,&size) && size.QuadPart>=0 && static_cast<ULONGLONG>(size.QuadPart)<=limit &&
            size.QuadPart<=std::numeric_limits<DWORD>::max(),"Invalid file size");
    std::string bytes(static_cast<std::size_t>(size.QuadPart),'\0'); DWORD received=0;
    require(ReadFile(file.handle,bytes.data(),static_cast<DWORD>(bytes.size()),&received,nullptr) && received==bytes.size(),"Could not read complete file");
    return bytes;
}
void write_file(const std::filesystem::path& path,std::string_view bytes) {
    require(bytes.size()<=std::numeric_limits<DWORD>::max(),"File is too large");
    GUID id{}; require(SUCCEEDED(CoCreateGuid(&id)),"Cannot generate file transaction ID");
    wchar_t suffix[40]{}; StringFromGUID2(id,suffix,40);
    const auto temp=std::filesystem::path(path.wstring()+L"."+suffix+L".tmp");
    bool created=false;
    try {
        {
            File file{CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
            if (file.handle==INVALID_HANDLE_VALUE) { throw std::runtime_error("Cannot create file transaction; Windows error "+std::to_string(GetLastError())); }
            created=true;
            DWORD written=0;
            require(WriteFile(file.handle,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) && written==bytes.size() &&
                    FlushFileBuffers(file.handle),"Cannot persist complete file");
        }
        DWORD error=ERROR_SUCCESS; bool published=false;
        for (int attempt=0; attempt<20; ++attempt) {
            if (MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) { published=true; break; }
            error=GetLastError(); if (!transient(error)) { break; } Sleep(5);
        }
        if (!published) { throw std::runtime_error("Cannot publish file; Windows error "+std::to_string(error)); }
    } catch (...) { if (created) { DeleteFileW(temp.c_str()); } throw; }
}
}
