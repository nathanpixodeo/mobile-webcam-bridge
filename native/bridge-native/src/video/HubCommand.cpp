#include "video/HubCommand.h"

#include <cmath>

#include <mwb/FrameProtocol.h>

#include "core/Json.h"
#include "core/Strings.h"

namespace mwb::native {

namespace {

PlaceholderKind RequireKind(const JsonValue& value) {
    const std::string* text = value.AsString();
    if (text == nullptr) throw HubCommandError("\"kind\" must be a string");
    const std::optional<PlaceholderKind> kind = ParsePlaceholderKind(*text);
    if (!kind) throw HubCommandError("unknown placeholder kind \"" + *text + "\"");
    return *kind;
}

// An even integer in [2, max]: NV12 needs even dimensions.
std::uint32_t RequireDimension(const JsonValue& document, std::string_view key, std::uint32_t max) {
    const JsonValue* value = document.Find(key);
    const double* number = value != nullptr ? value->AsNumber() : nullptr;
    const std::string name(key);
    if (number == nullptr) throw HubCommandError("\"" + name + "\" must be a number");
    if (*number < 2 || *number > max || std::floor(*number) != *number || static_cast<std::uint32_t>(*number) % 2 != 0) {
        throw HubCommandError("\"" + name + "\" must be an even integer from 2 to " + std::to_string(max));
    }
    return static_cast<std::uint32_t>(*number);
}

}  // namespace

HubCommand ParseHubCommand(std::string_view line) {
    JsonValue document;
    try {
        document = ParseJson(line);
    } catch (const JsonParseError& error) {
        throw HubCommandError(std::string("invalid JSON: ") + error.what());
    }
    if (document.AsObject() == nullptr) throw HubCommandError("a command must be a JSON object");

    const JsonValue* cmd = document.Find("cmd");
    const std::string* name = cmd != nullptr ? cmd->AsString() : nullptr;
    if (name == nullptr) throw HubCommandError("missing \"cmd\"");

    const JsonValue* kind = document.Find("kind");
    if (*name == "placeholder") {
        if (kind == nullptr) throw HubCommandError("\"placeholder\" needs \"kind\" (a kind or null)");
        if (kind->IsNull()) return ShowPlaceholderCommand{std::nullopt};
        return ShowPlaceholderCommand{RequireKind(*kind)};
    }
    if (*name == "loadPlaceholder") {
        if (kind == nullptr) throw HubCommandError("\"loadPlaceholder\" needs \"kind\"");
        const JsonValue* path = document.Find("path");
        const std::string* pathText = path != nullptr ? path->AsString() : nullptr;
        if (pathText == nullptr || pathText->empty()) throw HubCommandError("\"loadPlaceholder\" needs a non-empty \"path\"");
        return LoadPlaceholderCommand{RequireKind(*kind), ToWide(*pathText),
                                      RequireDimension(document, "width", mwb::frame::kMaxWidth),
                                      RequireDimension(document, "height", mwb::frame::kMaxHeight)};
    }
    if (*name == "ingest") {
        return SetIngestSizeCommand{RequireDimension(document, "width", mwb::frame::kMaxWidth),
                                    RequireDimension(document, "height", mwb::frame::kMaxHeight)};
    }
    throw HubCommandError("unknown command \"" + *name + "\"");
}

}  // namespace mwb::native
