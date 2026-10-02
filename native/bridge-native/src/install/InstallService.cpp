#include "install/InstallService.h"

#include <memory>
#include <optional>
#include <vector>

#include <mwb/FrameProtocol.h>
#include <mwb/Identifiers.h>

#include "core/Command.h"
#include "core/Json.h"
#include "core/Output.h"
#include "install/ComponentSteps.h"
#include "install/FileSteps.h"
#include "install/ProductRegistry.h"
#include "install/StepRunner.h"
#include "platform/Com.h"
#include "platform/OsVersion.h"
#include "platform/Paths.h"

namespace mwb::native {

namespace fs = std::filesystem;

namespace {

fs::path InstallRoot() { return ProgramFilesDirectory() / mwb::ids::kInstallSubdirectory; }

CommandOutcome ReportOutcome(const RunReport& report, const std::optional<fs::path>& installDir) {
    JsonWriter writer;
    writer.BeginObject().Field("ok", report.ok).Field("version", std::wstring_view(mwb::ids::kProductVersion));
    writer.Key("installDir");
    if (installDir) {
        writer.String(installDir->native());
    } else {
        writer.Null();
    }
    report.WriteSteps(writer);
    if (report.firstError) {
        writer.Key("error");
        WriteErrorObject(writer, *report.firstError);
    }
    writer.EndObject();
    return CommandOutcome{writer.Take(), report.ok ? ExitCode::Success : report.firstError->Exit()};
}

std::vector<StagedFile> FilesFor(CameraBackend backend, bool mic) {
    std::vector<StagedFile> files{{L"bridge-native.exe", true}};
    if (backend == CameraBackend::MediaFoundation) files.push_back({L"vcam-mf.dll", true});
    if (backend == CameraBackend::DirectShow) {
        files.push_back({L"vcam-dshow.dll", true});
        files.push_back({fs::path(L"x86") / L"vcam-dshow.dll", true});
    }
    if (mic) {
        files.push_back({fs::path(L"driver") / L"mwbmic.sys", true});
        files.push_back({fs::path(L"driver") / L"mwbmic.inf", true});
        files.push_back({fs::path(L"driver") / L"mwbmic.cat", false});  // absent in unsigned dev builds
    }
    return files;
}

}  // namespace

CommandOutcome FailureOutcome(const CommandError& error) {
    JsonWriter writer;
    writer.BeginObject().Field("ok", false).Key("error");
    WriteErrorObject(writer, error);
    writer.EndObject();
    return CommandOutcome{writer.Take(), error.Exit()};
}

InstallService::InstallService(Console& console, fs::path stageDir) : console_(console), stageDir_(std::move(stageDir)) {}

CommandOutcome InstallService::Install(const InstallOptions& options) {
    const ComApartment apartment;
    const CameraBackend backend = ResolveCameraBackend(options.camera, QueryOsVersion());
    if (backend == CameraBackend::None && !options.mic) {
        throw CommandError(ErrorCode::Usage, "Nothing to install: --camera none together with --no-mic");
    }

    const fs::path installDir = InstallRoot() / mwb::ids::kProductVersion;
    const ProductRegistry registry;
    const std::optional<ProductRegistration> previousProduct = registry.ReadProduct();
    const std::optional<CameraRegistration> previousCamera = registry.ReadCamera();

    InstallPlan plan;
    if (!IsSameDirectory(stageDir_, installDir)) {
        plan.push_back(std::make_unique<CopyFilesStep>(stageDir_, installDir, FilesFor(backend, options.mic)));
    }

    const bool replacesCamera =
        previousCamera && backend != CameraBackend::None &&
        (previousCamera->backend != backend ||
         (backend == CameraBackend::MediaFoundation && previousCamera->friendlyName != options.friendlyName));
    if (replacesCamera) {
        const fs::path previousDir = previousProduct ? fs::path(previousProduct->installDir) : installDir;
        plan.push_back(std::make_unique<RemovePreviousCameraStep>(*previousCamera, previousDir));
    }

    std::optional<CameraRegistration> camera;
    if (backend != CameraBackend::None) {
        camera = CameraRegistration{backend, options.friendlyName, options.defaultMode, options.cap,
                                    mwb::frame::kDefaultPublicPipeName};
    }
    plan.push_back(std::make_unique<WriteRegistryStep>(
        ProductRegistration{installDir.native(), mwb::ids::kProductVersion}, camera));

    if (backend == CameraBackend::MediaFoundation) {
        plan.push_back(std::make_unique<RegisterMfSourceStep>(installDir / L"vcam-mf.dll"));
        plan.push_back(std::make_unique<CreateVirtualCameraStep>(options.friendlyName));
    } else if (backend == CameraBackend::DirectShow) {
        plan.push_back(std::make_unique<RegisterDShowFilterStep>(installDir / L"vcam-dshow.dll",
                                                                 installDir / L"x86" / L"vcam-dshow.dll"));
    }
    if (options.mic) {
        plan.push_back(std::make_unique<InstallMicDriverStep>(installDir / L"driver" / L"mwbmic.inf"));
    }

    const RunReport report = StepRunner(console_).Run(plan, RunMode::Transactional);
    return ReportOutcome(report, installDir);
}

CommandOutcome InstallService::Uninstall(const UninstallOptions& options) {
    const ComApartment apartment;
    const ProductRegistry registry;
    const std::optional<ProductRegistration> product = registry.ReadProduct();
    const std::optional<CameraRegistration> camera = registry.ReadCamera();
    const fs::path installDir = product ? fs::path(product->installDir) : InstallRoot() / mwb::ids::kProductVersion;

    InstallPlan plan;
    if (options.camera) {
        const bool knownBackend = camera.has_value();
        if (!knownBackend || camera->backend == CameraBackend::MediaFoundation) {
            if (knownBackend) plan.push_back(std::make_unique<RemoveVirtualCameraStep>(camera->friendlyName));
            plan.push_back(std::make_unique<UnregisterMfSourceStep>(installDir / L"vcam-mf.dll"));
        }
        if (!knownBackend || camera->backend == CameraBackend::DirectShow) {
            plan.push_back(std::make_unique<UnregisterDShowFilterStep>(installDir / L"vcam-dshow.dll",
                                                                       installDir / L"x86" / L"vcam-dshow.dll"));
        }
        plan.push_back(std::make_unique<DeleteCameraRegistryStep>());
    }
    if (options.mic) plan.push_back(std::make_unique<UninstallMicDriverStep>());
    if (options.Everything()) {
        plan.push_back(std::make_unique<DeleteProductRegistryStep>());
        plan.push_back(std::make_unique<DeleteFilesStep>(InstallRoot()));
    }

    const RunReport report = StepRunner(console_).Run(plan, RunMode::BestEffort);
    return ReportOutcome(report, product ? std::optional<fs::path>(installDir) : std::nullopt);
}

}  // namespace mwb::native
