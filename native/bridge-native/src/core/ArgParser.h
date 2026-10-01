// Strict command-line option parser. Every command declares the options it accepts; anything
// else (unknown option, missing value, repeated option, stray positional) is a usage error.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mwb::native {

struct OptionSpec {
    std::wstring name;  // without the leading "--"
    bool takesValue = false;
    bool hidden = false;  // accepted but not advertised (e.g. --elevated)
};

class ParsedOptions {
public:
    [[nodiscard]] bool Has(std::wstring_view name) const;
    [[nodiscard]] std::optional<std::wstring> Value(std::wstring_view name) const;

    // Typed accessors; they throw CommandError(Usage) on malformed values.
    [[nodiscard]] std::optional<std::uint32_t> UInt32(std::wstring_view name, std::uint32_t min, std::uint32_t max) const;
    [[nodiscard]] std::wstring Required(std::wstring_view name) const;

    void Set(std::wstring name, std::optional<std::wstring> value);

private:
    std::map<std::wstring, std::optional<std::wstring>, std::less<>> values_;
};

class ArgParser {
public:
    ArgParser(std::initializer_list<OptionSpec> specs);

    // Parses `--name value`, `--name=value` and `--flag`. Throws CommandError(Usage).
    [[nodiscard]] ParsedOptions Parse(std::span<const std::wstring> args) const;

private:
    [[nodiscard]] const OptionSpec* Find(std::wstring_view name) const;

    std::vector<OptionSpec> specs_;
};

// Strict decimal parse: digits only, no sign, no leading '+', no overflow.
[[nodiscard]] std::optional<std::uint32_t> ParseDecimalUInt32(std::wstring_view text) noexcept;

}  // namespace mwb::native
