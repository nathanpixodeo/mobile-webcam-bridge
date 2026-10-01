#include "install/ProductRegistry.h"

#include <mwb/Identifiers.h>

#include "platform/Registry.h"

namespace mwb::native {

bool IsValidPipeName(std::wstring_view name) noexcept {
    if (name.empty() || name.size() > 128) return false;
    for (const wchar_t c : name) {
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' ||
                        c == L'_' || c == L'-';
        if (!ok) return false;
    }
    return true;
}

std::optional<ProductRegistration> ProductRegistry::ReadProduct() const {
    const std::optional<RegistryKey> key = RegistryKey::Open(HKEY_LOCAL_MACHINE, mwb::ids::kProductRegistryKey);
    if (!key) return std::nullopt;
    std::optional<std::wstring> installDir = key->GetString(L"InstallDir");
    if (!installDir || installDir->empty()) return std::nullopt;
    return ProductRegistration{*installDir, key->GetString(L"Version").value_or(L"")};
}

std::optional<CameraRegistration> ProductRegistry::ReadCamera() const {
    const std::optional<RegistryKey> key = RegistryKey::Open(HKEY_LOCAL_MACHINE, mwb::ids::kCameraRegistryKey);
    if (!key) return std::nullopt;

    const std::optional<std::wstring> backendText = key->GetString(L"Backend");
    const std::optional<CameraBackend> backend = backendText ? ParseCameraBackend(*backendText) : std::nullopt;
    if (!backend || *backend == CameraBackend::None) return std::nullopt;

    CameraRegistration camera;
    camera.backend = *backend;
    camera.friendlyName = key->GetString(L"FriendlyName").value_or(mwb::ids::kDefaultFriendlyName);
    camera.mode.width = key->GetDword(L"Width").value_or(0);
    camera.mode.height = key->GetDword(L"Height").value_or(0);
    camera.mode.fpsNum = key->GetDword(L"FpsNum").value_or(0);
    camera.mode.fpsDen = key->GetDword(L"FpsDen").value_or(0);
    camera.pipeName = key->GetString(L"PipeName").value_or(mwb::frame::kDefaultPublicPipeName);
    if (!mwb::frame::IsSupportedMode(camera.mode) || !IsValidPipeName(camera.pipeName)) return std::nullopt;
    return camera;
}

void ProductRegistry::WriteProduct(const ProductRegistration& product) const {
    RegistryKey key = RegistryKey::Create(HKEY_LOCAL_MACHINE, mwb::ids::kProductRegistryKey);
    key.SetString(L"InstallDir", product.installDir);
    key.SetString(L"Version", product.version);
}

void ProductRegistry::WriteCamera(const CameraRegistration& camera) const {
    RegistryKey key = RegistryKey::Create(HKEY_LOCAL_MACHINE, mwb::ids::kCameraRegistryKey);
    const std::string_view backend = ToString(camera.backend);
    key.SetString(L"Backend", std::wstring(backend.begin(), backend.end()));
    key.SetString(L"FriendlyName", camera.friendlyName);
    key.SetDword(L"Width", camera.mode.width);
    key.SetDword(L"Height", camera.mode.height);
    key.SetDword(L"FpsNum", camera.mode.fpsNum);
    key.SetDword(L"FpsDen", camera.mode.fpsDen);
    key.SetString(L"PipeName", camera.pipeName);
}

void ProductRegistry::DeleteCamera() const { RegistryKey::DeleteTree(HKEY_LOCAL_MACHINE, mwb::ids::kCameraRegistryKey); }

void ProductRegistry::DeleteAll() const { RegistryKey::DeleteTree(HKEY_LOCAL_MACHINE, mwb::ids::kProductRegistryKey); }

}  // namespace mwb::native
