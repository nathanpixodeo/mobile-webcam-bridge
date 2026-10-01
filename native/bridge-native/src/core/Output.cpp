#include "core/Output.h"

#include <windows.h>

#include <algorithm>
#include <string>

#include "core/Json.h"

namespace mwb::native {

bool OutputChannel::WriteLine(std::string_view line) {
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) return false;
    std::string buffer;
    buffer.reserve(line.size() + 1);
    buffer.append(line);
    buffer.push_back('\n');

    const std::lock_guard lock(mutex_);
    std::size_t offset = 0;
    while (offset < buffer.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(buffer.size() - offset, 1u << 20));
        if (!::WriteFile(handle_, buffer.data() + offset, chunk, &written, nullptr) || written == 0) return false;
        offset += written;
    }
    return true;
}

Console::Console()
    : out_(::GetStdHandle(STD_OUTPUT_HANDLE)), err_(::GetStdHandle(STD_ERROR_HANDLE)) {}

bool Console::Emit(JsonWriter& document) { return out_.WriteLine(document.Take()); }

void Console::Info(std::string_view message) { Diagnostic("info", message); }
void Console::Warn(std::string_view message) { Diagnostic("warn", message); }
void Console::Error(std::string_view message) { Diagnostic("error", message); }

void Console::Diagnostic(std::string_view level, std::string_view message) {
    std::string line = "[bridge-native] ";
    line.append(level).append(": ").append(message);
    err_.WriteLine(line);
}

}  // namespace mwb::native
