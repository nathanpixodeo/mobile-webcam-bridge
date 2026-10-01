// TraceLogging for the camera consumers (provider "MobileWebcamBridge.Camera",
// {A3295508-0440-40A8-8AFE-057AD6324B95}). The DLLs live inside Frame Server and third-party apps,
// where a debugger is rarely available; capture with e.g.
//   wpr -start <profile with the provider> / tracelog / WPA, or `logman` + `tracerpt`.
#pragma once

#include <format>
#include <string_view>
#include <utility>

namespace mwb::um::trace {

enum class Level { Error, Warning, Info, Verbose };

// Call from DllMain(DLL_PROCESS_ATTACH). `component` must be a string literal (it is kept).
void Register(const wchar_t* component) noexcept;

// Call from DllMain(DLL_PROCESS_DETACH) when the DLL is being unloaded dynamically.
void Unregister() noexcept;

// Routes every WIL failure report (RETURN_IF_FAILED & co.) to the provider. Module-local.
void InstallWilFailureLogging() noexcept;

void Write(Level level, std::wstring_view message) noexcept;

template <typename... Args>
void Writef(Level level, std::wformat_string<Args...> format, Args&&... args) noexcept {
    try {
        Write(level, std::format(format, std::forward<Args>(args)...));
    } catch (...) {
        // Tracing must never take the host process down.
    }
}

}  // namespace mwb::um::trace
