#include <windows.h>

#include <filesystem>
#include <string>

#include <mwb/FrameProtocol.h>
#include <mwb/Identifiers.h>

#include "core/Strings.h"
#include "doctor/DoctorCheck.h"
#include "doctor/SystemProbes.h"
#include "platform/Paths.h"
#include "platform/Registry.h"

namespace mwb::native {

std::string_view ToString(CheckStatus status) noexcept {
    switch (status) {
        case CheckStatus::Pass: return "pass";
        case CheckStatus::Warn: return "warn";
        case CheckStatus::Fail: return "fail";
    }
    return "fail";
}

namespace {

namespace fs = std::filesystem;

constexpr unsigned long kProblemUnsignedDriver = 52;  // CM_PROB_UNSIGNED_DRIVER

CheckResult Pass(std::string message) { return {CheckStatus::Pass, std::move(message)}; }
CheckResult Warn(std::string message) { return {CheckStatus::Warn, std::move(message)}; }
CheckResult Fail(std::string message) { return {CheckStatus::Fail, std::move(message)}; }

std::string Build(const OsVersion& os) { return std::to_string(os.build); }

// Path registered as InprocServer32 for `clsid` in `view`, if any.
std::optional<std::wstring> InprocServerPath(std::wstring_view clsid, RegistryView view) {
    const std::wstring key = L"SOFTWARE\\Classes\\CLSID\\" + std::wstring(clsid) + L"\\InprocServer32";
    const std::optional<RegistryKey> server = RegistryKey::Open(HKEY_LOCAL_MACHINE, key, KEY_READ, view);
    if (!server) return std::nullopt;
    return server->GetString(L"");
}

bool IsComServerRegistered(std::wstring_view clsid, RegistryView view) {
    const std::optional<std::wstring> path = InprocServerPath(clsid, view);
    return path && FileExists(fs::path(*path));
}

class OsBuildCheck final : public DoctorCheck {
public:
    std::string_view Id() const override { return "os.build"; }
    CheckResult Run(const DoctorContext& context) const override {
        const unsigned long build = context.os.build;
        if (build >= 22000) return Pass("Windows build " + Build(context.os) + ": Media Foundation virtual camera supported");
        if (build >= 19045) return Pass("Windows 10 build " + Build(context.os) + ": DirectShow camera backend");
        if (build >= 19041) return Warn("Windows 10 build " + Build(context.os) + ": update to 22H2 (build 19045)");
        return Fail("Windows build " + Build(context.os) + " is not supported");
    }
};

class FrameServerCheck final : public DoctorCheck {
public:
    std::string_view Id() const override { return "frameserver.service"; }
    CheckResult Run(const DoctorContext& context) const override {
        if (!context.os.IsWindows11OrLater()) return Pass("Not needed by the DirectShow backend");
        for (const wchar_t* name : {L"FrameServer", L"FrameServerMonitor"}) {
            const ServiceInfo service = QueryService(name);
            if (service.startType == ServiceStartType::Missing) return Fail("Service " + ToUtf8(name) + " is missing");
            if (service.startType == ServiceStartType::Disabled) {
                return Fail("Service " + ToUtf8(name) + " is disabled; the virtual camera cannot start");
            }
        }
        return Pass("Windows Camera Frame Server services are available");
    }
};

class CameraRegisteredCheck final : public DoctorCheck {
public:
    std::string_view Id() const override { return "camera.registered"; }
    CheckResult Run(const DoctorContext& context) const override {
        if (!context.camera) return Warn("Virtual camera is not installed; run `install`");
        if (context.camera->backend == CameraBackend::MediaFoundation) {
            return IsComServerRegistered(mwb::ids::kMfSourceClsid, RegistryView::Native64)
                       ? Pass("Media Foundation source is registered")
                       : Fail("Media Foundation source COM registration is missing or points to a missing file");
        }
        const bool x64 = IsComServerRegistered(mwb::ids::kDShowFilterClsid, RegistryView::Native64);
        const bool x86 = IsComServerRegistered(mwb::ids::kDShowFilterClsid, RegistryView::Wow32);
        if (x64 && x86) return Pass("DirectShow filter is registered (x64 and x86)");
        return Fail(std::string("DirectShow filter registration is incomplete (x64: ") + (x64 ? "yes" : "no") +
                    ", x86: " + (x86 ? "yes" : "no") + ")");
    }
};

class CameraFilesCheck final : public DoctorCheck {
public:
    std::string_view Id() const override { return "camera.files"; }
    CheckResult Run(const DoctorContext& context) const override {
        if (!context.product || !context.camera) return Warn("Virtual camera is not installed");
        const fs::path dir(context.product->installDir);
        std::vector<fs::path> files;
        if (context.camera->backend == CameraBackend::MediaFoundation) {
            files.push_back(dir / L"vcam-mf.dll");
        } else {
            files.push_back(dir / L"vcam-dshow.dll");
            files.push_back(dir / L"x86" / L"vcam-dshow.dll");
        }
        for (const fs::path& file : files) {
            if (!FileExists(file)) return Fail("Missing " + ToUtf8(file.native()));
            if (HasZoneIdentifier(file)) {
                return Fail(ToUtf8(file.native()) + " carries Mark-of-the-Web; reinstall or unblock the file");
            }
        }
        return Pass("Camera files are present in " + ToUtf8(dir.native()));
    }
};

class CameraPrivacyCheck final : public DoctorCheck {
public:
    std::string_view Id() const override { return "camera.privacy"; }
    CheckResult Run(const DoctorContext&) const override {
        if (QueryPrivacyConsent(L"webcam").Denied()) {
            return Fail("Camera access is turned off in Settings > Privacy & security > Camera");
        }
        return Pass("Camera access is allowed");
    }
};

class DuplicateBackendsCheck final : public DoctorCheck {
public:
    std::string_view Id() const override { return "camera.duplicateBackends"; }
    CheckResult Run(const DoctorContext& context) const override {
        const bool dshow = IsComServerRegistered(mwb::ids::kDShowFilterClsid, RegistryView::Native64) ||
                           IsComServerRegistered(mwb::ids::kDShowFilterClsid, RegistryView::Wow32);
        const bool mf = IsComServerRegistered(mwb::ids::kMfSourceClsid, RegistryView::Native64);
        if (context.os.IsWindows11OrLater() && dshow && mf) {
            return Warn("Both camera backends are registered, so apps list the camera twice; run `install --camera mf`");
        }
        return Pass("One camera backend registered at most");
    }
};

class MicDriverCheck final : public DoctorCheck {
public:
    std::string_view Id() const override { return "mic.driver"; }
    CheckResult Run(const DoctorContext& context) const override {
        return context.mic.installed ? Pass("Virtual microphone device is installed")
                                     : Warn("Virtual microphone is not installed; run `install`");
    }
};

class MicProblemCodeCheck final : public DoctorCheck {
public:
    std::string_view Id() const override { return "mic.problemCode"; }
    CheckResult Run(const DoctorContext& context) const override {
        if (!context.mic.installed) return Pass("No microphone device");
        if (!context.mic.present) return Warn("The microphone device node exists but is not present");
        if (context.mic.problemCode == 0) return Pass("Microphone device is working");
        if (context.mic.problemCode == kProblemUnsignedDriver) {
            return Fail("Windows refused the driver signature (code 52): enable test signing or install a signed build");
        }
        return Fail("Microphone device reports problem code " + std::to_string(context.mic.problemCode));
    }
};

class MicEndpointCheck final : public DoctorCheck {
public:
    std::string_view Id() const override { return "mic.endpoint"; }
    CheckResult Run(const DoctorContext& context) const override {
        const std::vector<AudioEndpointInfo> endpoints = FindBridgeCaptureEndpoints();
        for (const AudioEndpointInfo& endpoint : endpoints) {
            if (endpoint.active) return Pass("Capture endpoint \"" + ToUtf8(endpoint.friendlyName) + "\" is active");
        }
        if (!endpoints.empty()) {
            return Warn("Capture endpoint \"" + ToUtf8(endpoints.front().friendlyName) + "\" is disabled in Sound settings");
        }
        return context.mic.installed ? Fail("The driver is installed but Windows shows no "Mobile Webcam Microphone" capture endpoint")
                                     : Pass("Not installed");
    }
};

class MicPrivacyCheck final : public DoctorCheck {
public:
    std::string_view Id() const override { return "mic.privacy"; }
    CheckResult Run(const DoctorContext&) const override {
        if (QueryPrivacyConsent(L"microphone").Denied()) {
            return Fail("Microphone access is turned off in Settings > Privacy & security > Microphone");
        }
        return Pass("Microphone access is allowed");
    }
};

class TestSigningCheck final : public DoctorCheck {
public:
    std::string_view Id() const override { return "system.testSigning"; }
    CheckResult Run(const DoctorContext& context) const override {
        const std::optional<bool> enabled = IsTestSigningEnabled();
        if (!enabled) return Warn("Cannot determine the test-signing state");
        if (*enabled) return Pass("Test signing is on");
        if (context.mic.problemCode == kProblemUnsignedDriver) {
            return Fail("Test signing is off and the installed driver is not production-signed");
        }
        return Pass("Test signing is off");
    }
};

class SecureBootCheck final : public DoctorCheck {
public:
    std::string_view Id() const override { return "system.secureBoot"; }
    CheckResult Run(const DoctorContext&) const override {
        const std::optional<bool> enabled = IsSecureBootEnabled();
        if (!enabled) return Pass("Secure Boot state unknown (legacy BIOS or no access)");
        return Pass(*enabled ? "Secure Boot is on (test-signed drivers need it off)" : "Secure Boot is off");
    }
};

class PipeFreeCheck final : public DoctorCheck {
public:
    std::string_view Id() const override { return "pipe.free"; }
    CheckResult Run(const DoctorContext& context) const override {
        const std::wstring name = context.camera ? context.camera->pipeName : std::wstring(mwb::frame::kDefaultPublicPipeName);
        if (PipeExists(name)) return Warn("\\\\.\\pipe\\" + ToUtf8(name) + " exists: a video hub is already running");
        return Pass("\\\\.\\pipe\\" + ToUtf8(name) + " is free");
    }
};

}  // namespace

std::vector<std::unique_ptr<DoctorCheck>> CreateDoctorChecks() {
    std::vector<std::unique_ptr<DoctorCheck>> checks;
    checks.push_back(std::make_unique<OsBuildCheck>());
    checks.push_back(std::make_unique<FrameServerCheck>());
    checks.push_back(std::make_unique<CameraRegisteredCheck>());
    checks.push_back(std::make_unique<CameraFilesCheck>());
    checks.push_back(std::make_unique<CameraPrivacyCheck>());
    checks.push_back(std::make_unique<DuplicateBackendsCheck>());
    checks.push_back(std::make_unique<MicDriverCheck>());
    checks.push_back(std::make_unique<MicProblemCodeCheck>());
    checks.push_back(std::make_unique<MicEndpointCheck>());
    checks.push_back(std::make_unique<MicPrivacyCheck>());
    checks.push_back(std::make_unique<TestSigningCheck>());
    checks.push_back(std::make_unique<SecureBootCheck>());
    checks.push_back(std::make_unique<PipeFreeCheck>());
    return checks;
}

}  // namespace mwb::native
