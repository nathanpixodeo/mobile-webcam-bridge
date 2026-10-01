#include "video/HubCommand.h"

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
        return LoadPlaceholderCommand{RequireKind(*kind), ToWide(*pathText)};
    }
    throw HubCommandError("unknown command \"" + *name + "\"");
}

}  // namespace mwb::native
