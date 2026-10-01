// Well-known filesystem locations and small file helpers.
#pragma once

#include <filesystem>
#include <optional>

namespace mwb::native {

[[nodiscard]] std::filesystem::path ModulePath();
[[nodiscard]] std::filesystem::path ModuleDirectory();
[[nodiscard]] std::filesystem::path ProgramFilesDirectory();
[[nodiscard]] std::filesystem::path SystemDirectory();
// SysWOW64 on 64-bit Windows; nullopt when WOW64 is not available.
[[nodiscard]] std::optional<std::filesystem::path> SystemWow64Directory();

[[nodiscard]] bool FileExists(const std::filesystem::path& path) noexcept;

// Mark-of-the-Web: the "Zone.Identifier" alternate data stream added to downloaded files.
[[nodiscard]] bool HasZoneIdentifier(const std::filesystem::path& path) noexcept;
void RemoveZoneIdentifier(const std::filesystem::path& path) noexcept;

}  // namespace mwb::native
