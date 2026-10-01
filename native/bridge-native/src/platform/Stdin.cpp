#include "platform/Stdin.h"

#include <windows.h>

#include <algorithm>

namespace mwb::native {

StdinReader::StdinReader() : handle_(::GetStdHandle(STD_INPUT_HANDLE)) {}

std::size_t StdinReader::Read(void* buffer, std::size_t size) {
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE || size == 0) return 0;
    DWORD read = 0;
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20));
    // ERROR_BROKEN_PIPE (writer closed) and a zero-byte read both mean end of input.
    if (!::ReadFile(handle_, buffer, request, &read, nullptr)) return 0;
    return read;
}

}  // namespace mwb::native
