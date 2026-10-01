// Registration primitives for the two camera backends.
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace mwb::native {

// Calls DllRegisterServer / DllUnregisterServer of an in-process COM server.
class ComServerModule {
public:
    static void Register(const std::filesystem::path& dll);
    static void Unregister(const std::filesystem::path& dll);
};

// Windows 11 Media Foundation virtual camera (MFCreateVirtualCamera, loaded at run time so the
// executable still starts on Windows 10). A camera is keyed by its creation parameters, so the
// same friendly name addresses the same camera.
class MfVirtualCameraRegistrar {
public:
    // Throws CommandError(NotSupportedOs) when the API is unavailable.
    MfVirtualCameraRegistrar();
    ~MfVirtualCameraRegistrar();
    MfVirtualCameraRegistrar(const MfVirtualCameraRegistrar&) = delete;
    MfVirtualCameraRegistrar& operator=(const MfVirtualCameraRegistrar&) = delete;

    // Lifetime_System + Access_AllUsers (requires elevation); persists across reboots.
    void Create(std::wstring_view friendlyName) const;
    void Remove(std::wstring_view friendlyName) const;

private:
    struct Api;
    std::unique_ptr<Api> api_;
};

// DirectShow filter registration through the matching regsvr32 (System32 for x64,
// SysWOW64 for x86).
class DShowFilterRegistrar {
public:
    DShowFilterRegistrar(std::filesystem::path x64Dll, std::optional<std::filesystem::path> x86Dll);

    void Register() const;
    void Unregister() const;

private:
    std::filesystem::path x64Dll_;
    std::optional<std::filesystem::path> x86Dll_;
};

// Removes the COM class keys of `clsid` from both registry views; fallback when a DLL that
// should unregister itself is gone.
void DeleteComClassKeys(std::wstring_view clsid);

}  // namespace mwb::native
