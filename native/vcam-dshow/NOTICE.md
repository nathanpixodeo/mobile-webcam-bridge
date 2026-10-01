# vcam-dshow — third-party notices

`vcam-dshow.dll` is an original implementation built on the DirectShow BaseClasses.

| Component | Used for | License |
|---|---|---|
| DirectShow BaseClasses, vendored in `native/third_party/baseclasses` (from [microsoft/Windows-classic-samples](https://github.com/microsoft/Windows-classic-samples) via [tshino/softcam](https://github.com/tshino/softcam)) | `CSource`/`CSourceStream`, COM class factory, filter registration helpers | MIT, Copyright (c) Microsoft Corporation |
| [tshino/softcam](https://github.com/tshino/softcam) (`DShowSoftcam`, `softcam.cpp`) | Reference for the capture-pin interfaces a virtual camera needs (`IAMStreamConfig`, `IKsPropertySet` → `PIN_CATEGORY_CAPTURE`) and for registration in `CLSID_VideoInputDeviceCategory` | MIT, Copyright (c) 2020 tshino |

Differences from softcam: frames come from `bridge-native video hub` through the frame pipe
(`protocol/FRAME_PIPE.md`) instead of a shared-memory sender API; four pixel formats (YUY2, NV12,
I420, RGB24) instead of RGB24 only; paced delivery driven by frame arrival; stream-time
timestamps; `IAMPushSource`; dynamic format changes from video renderers.

The MIT license text is in `native/third_party/baseclasses/LICENSE` (Microsoft) and applies, with
the copyright line "Copyright (c) 2020 tshino", to the portions derived from softcam.
