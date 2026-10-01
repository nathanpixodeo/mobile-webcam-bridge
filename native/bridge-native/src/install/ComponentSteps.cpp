#include "install/ComponentSteps.h"

#include <mwb/Identifiers.h>

#include "install/CameraRegistrars.h"
#include "platform/Paths.h"

namespace mwb::native {

// --- registry ---------------------------------------------------------------------------------

WriteRegistryStep::WriteRegistryStep(ProductRegistration product, std::optional<CameraRegistration> camera)
    : product_(std::move(product)), camera_(std::move(camera)) {}

void WriteRegistryStep::Execute() {
    previousProduct_ = registry_.ReadProduct();
    previousCamera_ = registry_.ReadCamera();
    registry_.WriteProduct(product_);
    if (camera_) registry_.WriteCamera(*camera_);
}

bool WriteRegistryStep::Rollback() noexcept {
    try {
        if (previousProduct_) {
            registry_.WriteProduct(*previousProduct_);
        } else {
            registry_.DeleteAll();
        }
        if (previousCamera_) {
            registry_.WriteCamera(*previousCamera_);
        } else if (camera_ && previousProduct_) {
            registry_.DeleteCamera();
        }
        return true;
    } catch (...) {
        return false;
    }
}

// --- Media Foundation camera ----------------------------------------------------------------

void RegisterMfSourceStep::Execute() { ComServerModule::Register(dll_); }

bool RegisterMfSourceStep::Rollback() noexcept {
    try {
        ComServerModule::Unregister(dll_);
        return true;
    } catch (...) {
    }
    try {
        DeleteComClassKeys(mwb::ids::kMfSourceClsid);
        return true;
    } catch (...) {
        return false;
    }
}

void UnregisterMfSourceStep::Execute() {
    if (FileExists(dll_)) {
        try {
            ComServerModule::Unregister(dll_);
            return;
        } catch (...) {
            // Fall through to removing the keys directly.
        }
    }
    DeleteComClassKeys(mwb::ids::kMfSourceClsid);
}

void CreateVirtualCameraStep::Execute() { MfVirtualCameraRegistrar{}.Create(friendlyName_); }

bool CreateVirtualCameraStep::Rollback() noexcept {
    try {
        MfVirtualCameraRegistrar{}.Remove(friendlyName_);
        return true;
    } catch (...) {
        return false;
    }
}

void RemoveVirtualCameraStep::Execute() { MfVirtualCameraRegistrar{}.Remove(friendlyName_); }

// --- DirectShow camera ------------------------------------------------------------------------

RegisterDShowFilterStep::RegisterDShowFilterStep(std::filesystem::path x64Dll, std::optional<std::filesystem::path> x86Dll)
    : x64Dll_(std::move(x64Dll)), x86Dll_(std::move(x86Dll)) {}

void RegisterDShowFilterStep::Execute() { DShowFilterRegistrar(x64Dll_, x86Dll_).Register(); }

bool RegisterDShowFilterStep::Rollback() noexcept {
    try {
        DShowFilterRegistrar(x64Dll_, x86Dll_).Unregister();
    } catch (...) {
    }
    try {
        DeleteComClassKeys(mwb::ids::kDShowFilterClsid);
        return true;
    } catch (...) {
        return false;
    }
}

UnregisterDShowFilterStep::UnregisterDShowFilterStep(std::filesystem::path x64Dll,
                                                     std::optional<std::filesystem::path> x86Dll)
    : x64Dll_(std::move(x64Dll)), x86Dll_(std::move(x86Dll)) {}

void UnregisterDShowFilterStep::Execute() {
    try {
        DShowFilterRegistrar(x64Dll_, x86Dll_).Unregister();
    } catch (...) {
        // The DLLs may be gone already; removing the keys leaves the system clean either way.
    }
    DeleteComClassKeys(mwb::ids::kDShowFilterClsid);
}

RemovePreviousCameraStep::RemovePreviousCameraStep(CameraRegistration previous, std::filesystem::path previousInstallDir)
    : previous_(std::move(previous)), previousInstallDir_(std::move(previousInstallDir)) {}

void RemovePreviousCameraStep::Execute() {
    try {
        if (previous_.backend == CameraBackend::MediaFoundation) {
            MfVirtualCameraRegistrar{}.Remove(previous_.friendlyName);
        } else if (previous_.backend == CameraBackend::DirectShow) {
            DShowFilterRegistrar(previousInstallDir_ / L"vcam-dshow.dll", previousInstallDir_ / L"x86" / L"vcam-dshow.dll")
                .Unregister();
        }
    } catch (...) {
        // The previous camera may already be gone; nothing to undo then.
    }
    if (previous_.backend == CameraBackend::DirectShow) {
        try {
            DeleteComClassKeys(mwb::ids::kDShowFilterClsid);
        } catch (...) {
        }
    }
}

// --- microphone driver ------------------------------------------------------------------------

void InstallMicDriverStep::Execute() { change_ = driver_.Install(infPath_); }

bool InstallMicDriverStep::Rollback() noexcept {
    if (!change_.createdDeviceNode) return true;  // an existing node keeps the updated driver
    try {
        (void)driver_.Uninstall();
        return true;
    } catch (...) {
        return false;
    }
}

void UninstallMicDriverStep::Execute() { change_ = driver_.Uninstall(); }

}  // namespace mwb::native
