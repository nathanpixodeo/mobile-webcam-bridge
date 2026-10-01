#include "platform/Paths.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <string>

#include <wil/resource.h>
#include <wil/result.h>

namespace mwb::native {

std::filesystem::path ModulePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        THROW_LAST_ERROR_IF(length == 0);
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path ModuleDirectory() { return ModulePath().parent_path(); }

std::filesystem::path ProgramFilesDirectory() {
    wil::unique_cotaskmem_string path;
    THROW_IF_FAILED(::SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &path));
    return std::filesystem::path(path.get());
}

std::filesystem::path SystemDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = ::GetSystemDirectoryW(buffer, MAX_PATH);
    THROW_LAST_ERROR_IF(length == 0 || length >= MAX_PATH);
    return std::filesystem::path(buffer);
}

std::optional<std::filesystem::path> SystemWow64Directory() {
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = ::GetSystemWow64DirectoryW(buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return std::nullopt;
    return std::filesystem::path(buffer);
}

bool FileExists(const std::filesystem::path& path) noexcept {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool HasZoneIdentifier(const std::filesystem::path& path) noexcept {
    const std::wstring stream = path.native() + L":Zone.Identifier";
    return ::GetFileAttributesW(stream.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void RemoveZoneIdentifier(const std::filesystem::path& path) noexcept {
    const std::wstring stream = path.native() + L":Zone.Identifier";
    ::DeleteFileW(stream.c_str());
}

}  // namespace mwb::native
