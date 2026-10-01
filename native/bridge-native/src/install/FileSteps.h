// Steps that place or remove the product files under %ProgramFiles%\MobileWebcamBridge.
#pragma once

#include <filesystem>
#include <vector>

#include "install/InstallStep.h"

namespace mwb::native {

struct StagedFile {
    std::filesystem::path relativePath;
    bool required = true;
};

// Copies files from the stage folder into the versioned install folder. Existing files are
// renamed to backups first (renaming works even while a DLL is loaded), restored on rollback and
// deleted on commit; backups that are still locked are scheduled for deletion at reboot.
class CopyFilesStep final : public InstallStep {
public:
    CopyFilesStep(std::filesystem::path sourceDir, std::filesystem::path destinationDir, std::vector<StagedFile> files);

    [[nodiscard]] std::string Name() const override { return "copy-files"; }
    void Execute() override;
    bool Rollback() noexcept override;
    void Commit() noexcept override;
    [[nodiscard]] bool RebootRequired() const noexcept override { return rebootRequired_; }

private:
    struct Replacement {
        std::filesystem::path destination;
        std::filesystem::path backup;  // empty when the destination did not exist
    };

    std::filesystem::path sourceDir_;
    std::filesystem::path destinationDir_;
    std::vector<StagedFile> files_;
    std::vector<std::filesystem::path> createdDirectories_;
    std::vector<Replacement> replacements_;
    bool rebootRequired_ = false;
};

// Deletes a directory tree; files that are in use are scheduled for deletion at reboot.
class DeleteFilesStep final : public InstallStep {
public:
    explicit DeleteFilesStep(std::filesystem::path root);

    [[nodiscard]] std::string Name() const override { return "delete-files"; }
    void Execute() override;
    [[nodiscard]] bool RebootRequired() const noexcept override { return rebootRequired_; }

private:
    std::filesystem::path root_;
    bool rebootRequired_ = false;
};

// True when both paths name the same existing directory.
[[nodiscard]] bool IsSameDirectory(const std::filesystem::path& a, const std::filesystem::path& b) noexcept;

}  // namespace mwb::native
