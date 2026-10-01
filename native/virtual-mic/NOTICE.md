# Third-party notice — virtual-mic

`mwbmic.sys` is derived from the **Simple Audio Sample** driver of
[microsoft/Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples/tree/main/audio/simpleaudiosample)
(`audio/simpleaudiosample`), Copyright (c) Microsoft Corporation, licensed under the
**Microsoft Public License (MS-PL)**. The full license text is in
[`LICENSE-MS-PL.txt`](LICENSE-MS-PL.txt).

As MS-PL section 3(C)/(D) requires, every derived source file keeps Microsoft's copyright notice
and carries a note describing what was changed. Source distributions of this folder must include
`LICENSE-MS-PL.txt`. Binary distributions must be under a license that complies with MS-PL.

## Derived files (MS-PL)

| File | Derived from | Main changes |
|------|--------------|--------------|
| `src/adapter.cpp` | `Source/Main/adapter.cpp` | WDF and registry settings removed; single capture endpoint; IRP routing to the feed control device or PortCls |
| `src/common.cpp`, `src/common.h` | `Source/Main/common.cpp`, `Source/Inc/common.h` | mixer, WDF miniport, SaveData, ETW, subdevice cache and interface templates removed; adapter owns the feed buffer |
| `src/definitions.h` | `Source/Inc/definitions.h` | trimmed; MobileWebcamBridge pool tags |
| `src/endpoints.h` | `Source/Inc/endpoints.h` | capture pins only |
| `src/kshelper.cpp`, `src/kshelper.h` | `Source/Utilities/kshelper.cpp`, `Source/Inc/kshelper.h` | only `GetWaveFormatEx` and the basic-support handler remain |
| `src/mictopo.cpp`, `src/mictopo.h` | `Source/Main/basetopo.cpp`, `Source/Filters/micarraytopo.cpp` (+ headers) | merged; jack description only |
| `src/mictopotable.h` | `Source/Filters/micarray1toptable.h` | one microphone pin, no nodes |
| `src/micwavtable.h` | `Source/Filters/micarraywavtable.h` | 48 kHz / 16-bit / mono, default mode |
| `src/minipairs.h` | `Source/Filters/minipairs.h` | render endpoint removed |
| `src/minwavert.cpp`, `src/minwavert.h` | `Source/Main/minwavert.cpp`, `minwavert.h` | capture only; client format size validation |
| `src/minwavertstream.cpp`, `src/minwavertstream.h` | `Source/Main/minwavertstream.cpp`, `minwavertstream.h` | capture only; PCM comes from the feed buffer instead of a tone generator |
| `src/NewDelete.cpp`, `src/NewDelete.h` | `Source/Main/NewDelete.cpp`, `Source/Inc/NewDelete.h` | `noexcept` operators, tag-agnostic free |
| `mwbmic.inx` | `Source/Main/SimpleAudioSample.inx` | one capture endpoint, WDM, Windows 10 2004+ models section, no DRM attributes |

## Original files (not derived)

`src/ControlDevice.*`, `src/MicFeedBuffer.*`, `virtual-mic.rc`, `virtual-mic.vcxproj`,
`Directory.Build.props`, `KernelMode.props`, and the shared headers in `native/common/include/mwb/`.
