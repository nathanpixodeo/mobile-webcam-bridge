// Commands the video hub reads from stdin, one JSON object per line.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include "video/OutputPolicy.h"

namespace mwb::native {

struct LoadPlaceholderCommand {
    PlaceholderKind kind;
    std::wstring path;
    std::uint32_t width;   // frame::IsIngestSize
    std::uint32_t height;
};

struct ShowPlaceholderCommand {
    std::optional<PlaceholderKind> kind;  // nullopt: back to live frames
};

// New size of the raw frames on the ingest pipe: the size the phone encodes (frame::IsIngestSize),
// not necessarily a catalog size.
struct SetIngestSizeCommand {
    std::uint32_t width;
    std::uint32_t height;
};

using HubCommand = std::variant<LoadPlaceholderCommand, ShowPlaceholderCommand, SetIngestSizeCommand>;

class HubCommandError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Parses one command line. Unknown object keys are ignored (forward compatibility); everything
// else that does not match the contract throws HubCommandError.
[[nodiscard]] HubCommand ParseHubCommand(std::string_view line);

}  // namespace mwb::native
