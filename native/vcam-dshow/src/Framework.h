// Common includes for vcam-dshow.dll. Included first by every translation unit of this project.
#pragma once

// Our sources use std::min/std::max, so suppress the Windows macros here. The vendored BaseClasses
// .cpp files never include this header and keep the macros they rely on.
#ifndef NOMINMAX
#define NOMINMAX
#endif

// DirectShow BaseClasses (native/third_party/baseclasses). They predate /W4 /permissive-, so their
// headers are included at warning level 0; our own code keeps the project's /W4 /WX.
#pragma warning(push, 0)
#include <streams.h>
#pragma warning(pop)

#include <optional>
