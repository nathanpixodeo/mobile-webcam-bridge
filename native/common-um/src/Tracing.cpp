#include <mwb/um/Tracing.h>

#include <windows.h>

#include <TraceLoggingProvider.h>
#include <winmeta.h>

#include <wil/result_macros.h>

#include <algorithm>
#include <atomic>

// {A3295508-0440-40A8-8AFE-057AD6324B95}
TRACELOGGING_DEFINE_PROVIDER(g_ipbCameraProvider, "MobileWebcamBridge.Camera",
                             (0xa3295508, 0x0440, 0x40a8, 0x8a, 0xfe, 0x05, 0x7a, 0xd6, 0x32, 0x4b, 0x95));

namespace mwb::um::trace {

namespace {

std::atomic<bool> g_registered{false};
const wchar_t* g_component = L"camera";

constexpr std::size_t kMaxMessageChars = 2048;

void __stdcall OnWilFailure(const wil::FailureInfo& failure) noexcept {
    wchar_t message[1024];
    if (SUCCEEDED(wil::GetFailureLogString(message, ARRAYSIZE(message), failure))) {
        Write(Level::Warning, message);
    }
}

}  // namespace

void Register(const wchar_t* component) noexcept {
    g_component = component;
    // EventRegister (behind TraceLoggingRegister) is implemented in ntdll and safe under the
    // loader lock, so this may run from DllMain.
    if (!g_registered.exchange(true) && FAILED(TraceLoggingRegister(g_ipbCameraProvider))) {
        g_registered = false;
    }
}

void Unregister() noexcept {
    if (g_registered.exchange(false)) {
        TraceLoggingUnregister(g_ipbCameraProvider);
    }
}

void InstallWilFailureLogging() noexcept {
    wil::SetResultLoggingCallback(&OnWilFailure);
}

void Write(Level level, std::wstring_view message) noexcept {
    if (!g_registered.load(std::memory_order_relaxed)) return;
    const auto length = static_cast<USHORT>(std::min(message.size(), kMaxMessageChars));
    const wchar_t* text = message.data();

    // TraceLoggingLevel must be a compile-time constant, hence one write per level.
    switch (level) {
        case Level::Error:
            TraceLoggingWrite(g_ipbCameraProvider, "Message", TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
                              TraceLoggingWideString(g_component, "Component"),
                              TraceLoggingCountedWideString(text, length, "Text"));
            break;
        case Level::Warning:
            TraceLoggingWrite(g_ipbCameraProvider, "Message", TraceLoggingLevel(WINEVENT_LEVEL_WARNING),
                              TraceLoggingWideString(g_component, "Component"),
                              TraceLoggingCountedWideString(text, length, "Text"));
            break;
        case Level::Info:
            TraceLoggingWrite(g_ipbCameraProvider, "Message", TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                              TraceLoggingWideString(g_component, "Component"),
                              TraceLoggingCountedWideString(text, length, "Text"));
            break;
        case Level::Verbose:
            TraceLoggingWrite(g_ipbCameraProvider, "Message", TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                              TraceLoggingWideString(g_component, "Component"),
                              TraceLoggingCountedWideString(text, length, "Text"));
            break;
    }
}

}  // namespace mwb::um::trace
