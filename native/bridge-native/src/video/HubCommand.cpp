#include "video/HubCommand.h"

#include <cmath>
#include <utility>

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

std::uint32_t RequireInteger(const JsonValue& document, std::string_view key) {
    const JsonValue* value = document.Find(key);
    const double* number = value != nullptr ? value->AsNumber() : nullptr;
    const std::string name(key);
    if (number == nullptr || *number < 0 || *number > 65536 || std::floor(*number) != *number) {
        throw HubCommandError("\"" + name + "\" must be a non-negative integer");
    }
    return static_cast<std::uint32_t>(*number);
}

// A frame size the hub can hold: see frame::IsIngestSize.
std::pair<std::uint32_t, std::uint32_t> RequireFrameSize(const JsonValue& document) {
    const std::uint32_t width = RequireInteger(document, "width");
    const std::uint32_t height = RequireInteger(document, "height");
    if (!mwb::frame::IsIngestSize(width, height)) {
        throw HubCommandError("\"width\" and \"height\" must be even, at most 3840 each and 3840x2160 pixels");
    }
    return {width, height};
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
        const auto [width, height] = RequireFrameSize(document);
        return LoadPlaceholderCommand{RequireKind(*kind), ToWide(*pathText), width, height};
    }
    if (*name == "ingest") {
        const auto [width, height] = RequireFrameSize(document);
        return SetIngestSizeCommand{width, height};
    }
    throw HubCommandError("unknown command \"" + *name + "\"");
}

}  // namespace mwb::native
