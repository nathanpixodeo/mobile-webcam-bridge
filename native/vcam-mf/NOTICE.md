# vcam-mf — third-party notices

`vcam-mf.dll` is an original implementation. Its structure and Media Foundation state semantics were
studied from, and in places follow, these MIT-licensed samples:

| Source | Used for | License |
|---|---|---|
| [microsoft/Windows-Camera — Samples/VirtualCamera](https://github.com/microsoft/Windows-Camera/tree/master/Samples/VirtualCamera) (`SimpleMediaSource`, `SimpleMediaStream`, `VirtualCameraMediaSourceActivate`) | Source/stream state machine, event sequence (`MENewStream` → `MEStreamStarted` → `MESourceStarted`), provided-allocator usage, sensor profile, `IKsControl` "not supported" convention | MIT, Copyright (c) Microsoft Corporation. All rights reserved. |
| [smourier/VCamSample](https://github.com/smourier/VCamSample) (`MediaSource`, `MediaStream`, `Activator`, `dllmain`) | HKLM-only COM registration, activator attributes | MIT, Copyright (c) 2024-2026 Simon Mourier |

Deliberate differences from VCamSample, which together are the suspected cause of its known
"Teams preview works but the remote party sees nothing" issue:

- `SetStreamState` compares (`==`) instead of assigning (`=`) the state.
- NV12 `MF_MT_DEFAULT_STRIDE` is the luma row (`width`), not `width × 1.5`.
- Sample requests are paced (`FrameDelivery`) instead of answered as fast as they arrive.
- 16:9 modes from the installed configuration instead of a fixed 1280×960.
- No Direct2D/DirectWrite/WIC frame generator and no undocumented interfaces; frames come from
  `bridge-native video hub` through the frame pipe (`protocol/FRAME_PIPE.md`).

## MIT License (applies to the portions derived from the samples above)

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
