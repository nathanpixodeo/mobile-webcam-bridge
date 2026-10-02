// JSON shapes of camera modes shared by `status` and the video hub events
// (protocol/BRIDGE_NATIVE.md).
#pragma once

#include <mwb/FrameProtocol.h>

#include "core/Json.h"

namespace mwb::native {

// {"width":…,"height":…,"fpsNum":…,"fpsDen":…}
inline JsonWriter& WriteMode(JsonWriter& writer, const mwb::frame::VideoMode& mode) {
    return writer.BeginObject()
        .Field("width", mode.width)
        .Field("height", mode.height)
        .Field("fpsNum", mode.fpsNum)
        .Field("fpsDen", mode.fpsDen)
        .EndObject();
}

// The mode's fields inline, for objects that carry one mode next to other fields.
inline JsonWriter& WriteModeFields(JsonWriter& writer, const mwb::frame::VideoMode& mode) {
    return writer.Field("width", mode.width)
        .Field("height", mode.height)
        .Field("fpsNum", mode.fpsNum)
        .Field("fpsDen", mode.fpsDen);
}

inline JsonWriter& WriteCapFields(JsonWriter& writer, const mwb::frame::ModeCap& cap) {
    return writer.Field("maxWidth", cap.maxWidth).Field("maxHeight", cap.maxHeight).Field("maxFps", cap.maxFps);
}

}  // namespace mwb::native
