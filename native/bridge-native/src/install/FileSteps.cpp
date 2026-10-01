#include "install/FileSteps.h"

#include <windows.h>

#include <algorithm>
#include <system_error>

#include <mwb/Identifiers.h>

#include "core/Errors.h"
#include "core/Strings.h"
#include "platform/Paths.h"

namespace mwb::native {

namespace fs = std::filesystem;

namespace {

bool DirectoryExists(const fs::path& path) noexcept {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Creates `directory` and any missing parents, appending each directory it created to `created`
// (outermost first).
void CreateDirectoriesTracked(const fs::path& directory, std::vector<fs::path>& created) {
    std::vector<fs::path> missing;
    for (fs::path current = directory; !current.empty() && !DirectoryExists(current); current = current.parent_path()) {
        missing.push_back(current);
        if (current == current.parent_path()) break;
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        if (!::CreateDirectoryW(it->c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
            throw ErrorFromWin32(::GetLastError(), "Cannot create " + ToUtf8(it->native()));
        }
        created.push_back(*it);
    }
}

// Deletes a file now, or at the next reboot when it is in use. Returns false if neither worked.
bool DeleteNowOrAtReboot(const fs::path& file, bool& scheduled) noexcept {
    ::SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (::DeleteFileW(file.c_str())) return true;
    if (::MoveFileExW(file.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        scheduled = true;
        return true;
    }
    return false;
}

}  // namespace

bool IsSameDirectory(const fs::path& a, const fs::path& b) noexcept {
    std::error_code error;
    const bool same = fs::equivalent(a, b, error);
    return !error && same;
}

// ---------------------------------------------------------------------------------------------

CopyFilesStep::CopyFilesStep(fs::path sourceDir, fs::path destinationDir, std::vector<StagedFile> files)
    : sourceDir_(std::move(sourceDir)), destinationDir_(std::move(destinationDir)), files_(std::move(files)) {}

void CopyFilesStep::Execute() {
    for (const StagedFile& file : files_) {
        if (file.required && !FileExists(sourceDir_ / file.relativePath)) {
            throw CommandError(ErrorCode::IoError, "Missing build output " + ToUtf8((sourceDir_ / file.relativePath).native()) +
                                                       "; run scripts/build-native.ps1 first");
        }
    }

    try {
        CreateDirectoriesTracked(destinationDir_, createdDirectories_);
        for (const StagedFile& file : files_) {
            const fs::path source = sourceDir_ / file.relativePath;
            if (!FileExists(source)) continue;  // optional file not built
            const fs::path destination = destinationDir_ / file.relativePath;
            CreateDirectoriesTracked(destination.parent_path(), createdDirectories_);

            Replacement replacement{destination, {}};
            if (FileExists(destination)) {
                replacement.backup = destination;
                replacement.backup += L".bak-" + RandomHexToken(4);
                if (!::MoveFileExW(destination.c_str(), replacement.backup.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    throw ErrorFromWin32(::GetLastError(), "Cannot replace " + ToUtf8(destination.native()));
                }
            }
            replacements_.push_back(replacement);

            if (!::CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
                throw ErrorFromWin32(::GetLastError(), "Cannot copy " + ToUtf8(file.relativePath.native()));
            }
            // A downloaded build carries Mark-of-the-Web; Frame Server refuses such DLLs.
            RemoveZoneIdentifier(destination);
        }
    } catch (...) {
        (void)Rollback();
        throw;
    }
}

bool CopyFilesStep::Rollback() noexcept {
    bool ok = true;
    for (auto it = replacements_.rbegin(); it != replacements_.rend(); ++it) {
        bool scheduled = false;
        if (FileExists(it->destination) && !DeleteNowOrAtReboot(it->destination, scheduled)) ok = false;
        if (!it->backup.empty() &&
            !::MoveFileExW(it->backup.c_str(), it->destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            ok = false;
        }
    }
    replacements_.clear();
    for (auto it = createdDirectories_.rbegin(); it != createdDirectories_.rend(); ++it) {
        ::RemoveDirectoryW(it->c_str());
    }
    createdDirectories_.clear();
    return ok;
}

void CopyFilesStep::Commit() noexcept {
    for (const Replacement& replacement : replacements_) {
        if (replacement.backup.empty()) continue;
        bool scheduled = false;
        (void)DeleteNowOrAtReboot(replacement.backup, scheduled);
    }
    replacements_.clear();
    createdDirectories_.clear();
}

// ---------------------------------------------------------------------------------------------

DeleteFilesStep::DeleteFilesStep(fs::path root) : root_(std::move(root)) {}

void DeleteFilesStep::Execute() {
    // Guard against ever deleting anything but %ProgramFiles%\MobileWebcamBridge.
    if (root_.filename() != fs::path(mwb::ids::kInstallSubdirectory) || !IsSameDirectory(root_.parent_path(), ProgramFilesDirectory())) {
        throw CommandError(ErrorCode::Internal, "Refusing to delete unexpected directory " + ToUtf8(root_.native()));
    }
    if (!DirectoryExists(root_)) return;

    std::vector<fs::path> files;
    std::vector<fs::path> directories;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root_, error), end; !error && it != end; it.increment(error)) {
        if (it->is_directory(error)) {
            directories.push_back(it->path());
        } else {
            files.push_back(it->path());
        }
    }
    if (error) throw CommandError(ErrorCode::IoError, "Cannot list " + ToUtf8(root_.native()) + ": " + error.message());

    for (const fs::path& file : files) {
        if (!DeleteNowOrAtReboot(file, rebootRequired_)) {
            throw ErrorFromWin32(::GetLastError(), "Cannot delete " + ToUtf8(file.native()));
        }
    }
    // Deepest directories first; a directory still holding files pending deletion is removed at
    // reboot (pending operations run in the order they were scheduled).
    std::sort(directories.begin(), directories.end(),
              [](const fs::path& a, const fs::path& b) { return a.native().size() > b.native().size(); });
    directories.push_back(root_);
    for (const fs::path& directory : directories) {
        if (!::RemoveDirectoryW(directory.c_str()) &&
            ::MoveFileExW(directory.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
            rebootRequired_ = true;
        }
    }
}

}  // namespace mwb::native
