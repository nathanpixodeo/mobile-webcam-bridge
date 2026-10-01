// Process helpers: elevation state, Windows command-line quoting, run-and-wait.
#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace mwb::native {

[[nodiscard]] bool IsProcessElevated();

// Quotes one argument so CommandLineToArgvW / the CRT parse it back unchanged.
[[nodiscard]] std::wstring QuoteArgument(std::wstring_view argument);
[[nodiscard]] std::wstring JoinArguments(std::span<const std::wstring> arguments);

// Starts `executable` with `arguments` (no window) and waits up to `timeoutMs`.
// Returns the exit code; throws CommandError if it cannot start or times out.
[[nodiscard]] unsigned long RunAndWait(const std::filesystem::path& executable, std::span<const std::wstring> arguments,
                                       unsigned long timeoutMs);

}  // namespace mwb::native
