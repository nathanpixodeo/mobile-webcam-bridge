// HKLM\SOFTWARE\MobileWebcamBridge (product) and ...\Camera (camera mode), always in the 64-bit view.
// Layout: native/common/include/mwb/Identifiers.h.
#pragma once

#include <optional>
#include <string>

#include <mwb/FrameProtocol.h>

#include "install/InstallOptions.h"

namespace mwb::native {

struct ProductRegistration {
    std::wstring installDir;
    std::wstring version;
};

struct CameraRegistration {
    CameraBackend backend = CameraBackend::None;
    std::wstring friendlyName;
    mwb::frame::VideoMode mode{};
    std::wstring pipeName;
};

class ProductRegistry {
public:
    [[nodiscard]] std::optional<ProductRegistration> ReadProduct() const;
    // nullopt when the key is missing or holds an invalid/unsupported configuration.
    [[nodiscard]] std::optional<CameraRegistration> ReadCamera() const;

    void WriteProduct(const ProductRegistration& product) const;
    void WriteCamera(const CameraRegistration& camera) const;

    void DeleteCamera() const;
    void DeleteAll() const;
};

// Pipe names allowed in the registry / on the command line: [A-Za-z0-9._-]{1,128}.
[[nodiscard]] bool IsValidPipeName(std::wstring_view name) noexcept;

}  // namespace mwb::native
