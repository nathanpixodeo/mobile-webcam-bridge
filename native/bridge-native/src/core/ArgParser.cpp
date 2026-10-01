#include "core/ArgParser.h"

#include "core/Errors.h"
#include "core/Strings.h"

namespace mwb::native {

std::optional<std::uint32_t> ParseDecimalUInt32(std::wstring_view text) noexcept {
    if (text.empty() || text.size() > 10) return std::nullopt;
    std::uint64_t value = 0;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(c - L'0');
    }
    if (value > 0xFFFFFFFFull) return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

bool ParsedOptions::Has(std::wstring_view name) const { return values_.find(name) != values_.end(); }

std::optional<std::wstring> ParsedOptions::Value(std::wstring_view name) const {
    const auto it = values_.find(name);
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

std::optional<std::uint32_t> ParsedOptions::UInt32(std::wstring_view name, std::uint32_t min, std::uint32_t max) const {
    const std::optional<std::wstring> text = Value(name);
    if (!text) return std::nullopt;
    const std::optional<std::uint32_t> value = ParseDecimalUInt32(*text);
    if (!value || *value < min || *value > max) {
        throw CommandError(ErrorCode::Usage, "--" + ToUtf8(name) + " expects an integer between " + std::to_string(min) +
                                                 " and " + std::to_string(max));
    }
    return value;
}

std::wstring ParsedOptions::Required(std::wstring_view name) const {
    std::optional<std::wstring> value = Value(name);
    if (!value || value->empty()) throw CommandError(ErrorCode::Usage, "missing required option --" + ToUtf8(name));
    return *value;
}

void ParsedOptions::Set(std::wstring name, std::optional<std::wstring> value) {
    values_.insert_or_assign(std::move(name), std::move(value));
}

ArgParser::ArgParser(std::initializer_list<OptionSpec> specs) : specs_(specs) {}

const OptionSpec* ArgParser::Find(std::wstring_view name) const {
    for (const OptionSpec& spec : specs_) {
        if (spec.name == name) return &spec;
    }
    return nullptr;
}

ParsedOptions ArgParser::Parse(std::span<const std::wstring> args) const {
    ParsedOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring_view arg = args[i];
        if (!StartsWith(arg, L"--") || arg.size() == 2) {
            throw CommandError(ErrorCode::Usage, "unexpected argument '" + ToUtf8(arg) + "'");
        }

        std::wstring_view name = arg.substr(2);
        std::optional<std::wstring> inlineValue;
        if (const std::size_t eq = name.find(L'='); eq != std::wstring_view::npos) {
            inlineValue = std::wstring(name.substr(eq + 1));
            name = name.substr(0, eq);
        }

        const OptionSpec* spec = Find(name);
        if (spec == nullptr) throw CommandError(ErrorCode::Usage, "unknown option --" + ToUtf8(name));
        if (options.Has(name)) throw CommandError(ErrorCode::Usage, "option --" + ToUtf8(name) + " given twice");

        if (!spec->takesValue) {
            if (inlineValue) throw CommandError(ErrorCode::Usage, "option --" + ToUtf8(name) + " takes no value");
            options.Set(std::wstring(name), std::nullopt);
            continue;
        }
        if (inlineValue) {
            options.Set(std::wstring(name), std::move(inlineValue));
            continue;
        }
        if (i + 1 >= args.size() || StartsWith(args[i + 1], L"--")) {
            throw CommandError(ErrorCode::Usage, "option --" + ToUtf8(name) + " needs a value");
        }
        options.Set(std::wstring(name), args[++i]);
    }
    return options;
}

}  // namespace mwb::native
