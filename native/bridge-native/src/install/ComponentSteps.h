// Install/uninstall steps for the registry, the camera backends and the microphone driver.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "install/InstallStep.h"
#include "install/MicDriver.h"
#include "install/ProductRegistry.h"

namespace mwb::native {

// --- registry ---------------------------------------------------------------------------------

class WriteRegistryStep final : public InstallStep {
public:
    WriteRegistryStep(ProductRegistration product, std::optional<CameraRegistration> camera);

    [[nodiscard]] std::string Name() const override { return "write-registry"; }
    void Execute() override;
    bool Rollback() noexcept override;

private:
    ProductRegistry registry_;
    ProductRegistration product_;
    std::optional<CameraRegistration> camera_;
    std::optional<ProductRegistration> previousProduct_;
    std::optional<CameraRegistration> previousCamera_;
};

class DeleteCameraRegistryStep final : public InstallStep {
public:
    [[nodiscard]] std::string Name() const override { return "delete-camera-registry"; }
    void Execute() override { ProductRegistry{}.DeleteCamera(); }
};

class DeleteProductRegistryStep final : public InstallStep {
public:
    [[nodiscard]] std::string Name() const override { return "delete-registry"; }
    void Execute() override { ProductRegistry{}.DeleteAll(); }
};

// --- Media Foundation camera ----------------------------------------------------------------

class RegisterMfSourceStep final : public InstallStep {
public:
    explicit RegisterMfSourceStep(std::filesystem::path dll) : dll_(std::move(dll)) {}

    [[nodiscard]] std::string Name() const override { return "register-mf-source"; }
    void Execute() override;
    bool Rollback() noexcept override;

private:
    std::filesystem::path dll_;
};

class UnregisterMfSourceStep final : public InstallStep {
public:
    explicit UnregisterMfSourceStep(std::filesystem::path dll) : dll_(std::move(dll)) {}

    [[nodiscard]] std::string Name() const override { return "unregister-mf-source"; }
    void Execute() override;

private:
    std::filesystem::path dll_;
};

class CreateVirtualCameraStep final : public InstallStep {
public:
    explicit CreateVirtualCameraStep(std::wstring friendlyName) : friendlyName_(std::move(friendlyName)) {}

    [[nodiscard]] std::string Name() const override { return "create-virtual-camera"; }
    void Execute() override;
    bool Rollback() noexcept override;

private:
    std::wstring friendlyName_;
};

class RemoveVirtualCameraStep final : public InstallStep {
public:
    explicit RemoveVirtualCameraStep(std::wstring friendlyName) : friendlyName_(std::move(friendlyName)) {}

    [[nodiscard]] std::string Name() const override { return "remove-virtual-camera"; }
    void Execute() override;

private:
    std::wstring friendlyName_;
};

// --- DirectShow camera ------------------------------------------------------------------------

class RegisterDShowFilterStep final : public InstallStep {
public:
    RegisterDShowFilterStep(std::filesystem::path x64Dll, std::optional<std::filesystem::path> x86Dll);

    [[nodiscard]] std::string Name() const override { return "register-dshow-filter"; }
    void Execute() override;
    bool Rollback() noexcept override;

private:
    std::filesystem::path x64Dll_;
    std::optional<std::filesystem::path> x86Dll_;
};

class UnregisterDShowFilterStep final : public InstallStep {
public:
    UnregisterDShowFilterStep(std::filesystem::path x64Dll, std::optional<std::filesystem::path> x86Dll);

    [[nodiscard]] std::string Name() const override { return "unregister-dshow-filter"; }
    void Execute() override;

private:
    std::filesystem::path x64Dll_;
    std::optional<std::filesystem::path> x86Dll_;
};

// Removes a camera registered by an earlier install whose backend or friendly name differs from
// the new one, so no orphaned camera stays behind. Failures are tolerated (logged by the runner
// only if they throw, which this step avoids).
class RemovePreviousCameraStep final : public InstallStep {
public:
    RemovePreviousCameraStep(CameraRegistration previous, std::filesystem::path previousInstallDir);

    [[nodiscard]] std::string Name() const override { return "remove-previous-camera"; }
    void Execute() override;

private:
    CameraRegistration previous_;
    std::filesystem::path previousInstallDir_;
};

// --- microphone driver ------------------------------------------------------------------------

class InstallMicDriverStep final : public InstallStep {
public:
    explicit InstallMicDriverStep(std::filesystem::path infPath) : infPath_(std::move(infPath)) {}

    [[nodiscard]] std::string Name() const override { return "install-mic-driver"; }
    void Execute() override;
    bool Rollback() noexcept override;
    [[nodiscard]] bool RebootRequired() const noexcept override { return change_.rebootRequired; }

private:
    std::filesystem::path infPath_;
    MicDriver driver_;
    DriverChange change_;
};

class UninstallMicDriverStep final : public InstallStep {
public:
    [[nodiscard]] std::string Name() const override { return "uninstall-mic-driver"; }
    void Execute() override;
    [[nodiscard]] bool RebootRequired() const noexcept override { return change_.rebootRequired; }

private:
    MicDriver driver_;
    DriverChange change_;
};

}  // namespace mwb::native
