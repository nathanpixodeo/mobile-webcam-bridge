// bridge-native.exe — native helper of the Mobile Webcam Bridge host. Contract: protocol/BRIDGE_NATIVE.md.
#include <windows.h>

#include <span>
#include <string>
#include <vector>

#include "commands/Commands.h"
#include "core/Output.h"

int wmain(int argc, wchar_t** argv) {
    // Never let a hard error pop up a dialog: this process normally runs without a console user.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    mwb::native::Console console;
    mwb::native::Application application(console);
    application.Register(mwb::native::MakeVersionCommand());
    application.Register(mwb::native::MakeStatusCommand());
    application.Register(mwb::native::MakeInstallCommand());
    application.Register(mwb::native::MakeUninstallCommand());
    application.Register(mwb::native::MakeDoctorCommand());
    application.Register(mwb::native::MakeVideoHubCommand());
    application.Register(mwb::native::MakeVideoWatchCommand());
    application.Register(mwb::native::MakeMicFeedCommand());
    application.Register(mwb::native::MakeMicStatusCommand());

    const std::vector<std::wstring> args(argv + 1, argv + argc);
    return application.Run(std::span<const std::wstring>(args));
}
