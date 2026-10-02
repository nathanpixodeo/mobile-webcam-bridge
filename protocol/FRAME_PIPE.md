# Frame pipe protocol — version 2

Raw video moves from the decoder to the virtual camera components through two named pipes owned
by `bridge-native.exe video hub` (the **hub**). Frame bytes never pass through Node.

```
ffmpeg ──raw NV12──► ingest pipe ──► hub ──scale per consumer──► public pipe ──► vcam-mf.dll / vcam-dshow.dll
                                      ▲                                │
                                      └──── subscribe (mode) ◄─────────┘
```

C/C++ definition: `native/common/include/mwb/FrameProtocol.h`.

Version 2 adds the consumer subscription (§2.1): every consumer asks for one camera mode and
receives frames of exactly that size. Version 1 had one installed mode for everybody.

## 1. Camera modes

The virtual camera advertises a fixed catalog of modes, filtered by the installed cap (registry
`MaxWidth`, `MaxHeight`, `MaxFps`). Applications pick one when they open the camera.

| Size        | Frame rates (fps) |
|-------------|-------------------|
| 640 × 360   | 15, 30, 60        |
| 640 × 480   | 15, 30, 60        |
| 960 × 540   | 15, 30, 60        |
| 1280 × 720  | 15, 30, 60        |
| 1920 × 1080 | 15, 30, 60        |
| 2560 × 1440 | 15, 30, 60        |
| 3840 × 2160 | 15, 30            |

- Frame rates are integers (`fpsDen == 1`).
- 3840 × 2160 at 60 fps is not offered: raw NV12 at that rate is about 750 MB/s per consumer.
- A mode is **advertised** when it is in the catalog and `width ≤ MaxWidth`, `height ≤ MaxHeight`,
  `fps ≤ MaxFps`. The installed default mode (registry `Width`, `Height`, `FpsNum`, `FpsDen`) is
  advertised first, then the other modes from the largest to the smallest size and from the
  highest to the lowest frame rate.

## 2. Ingest pipe (decoder → hub)

- Name: `\\.\pipe\mobile-webcam-bridge-ingest-<token>`; Node generates `<token>` (16 random hex chars)
  and passes the full name to the hub with `--ingest-pipe`.
- Created by the hub with a DACL that grants access only to SYSTEM, Administrators and the
  hub's own user SID, plus `PIPE_REJECT_REMOTE_CLIENTS`.
- Content: raw NV12 frames back to back, no headers. One frame = `width × height × 3 / 2` bytes
  for the current **ingest size**. Rows are tightly packed (stride = width).
- The ingest size starts as the installed default mode and changes with the hub's `ingest` command
  (protocol/BRIDGE_NATIVE.md §3). The command drops the current writer and any partial frame.
- One writer at a time. When the writer disconnects, the hub discards any partial frame and
  waits for the next writer (ffmpeg is restarted on every decoder restart).

## 3. Public pipe (hub ↔ camera components)

- Name: `\\.\pipe\mobile-webcam-bridge-video` (override: registry `PipeName`, hub `--pipe-name`).
- Created by the hub with:
  - SDDL `D:P(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;<hubUserSid>)(A;;0x12008b;;;LS)`
    (deny network logons; LocalService — the Frame Server account — may read and write data
    (`FILE_GENERIC_READ | FILE_WRITE_DATA`), which it needs for the subscription),
  - `PIPE_REJECT_REMOTE_CLIENTS`,
  - `FILE_FLAG_FIRST_PIPE_INSTANCE` on the first instance, so a squatted name is an error.
- Byte mode. Clients open with `GENERIC_READ | FILE_WRITE_DATA` and must pass
  `SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS` to `CreateFileW`.
- A connected client becomes a **consumer** once its subscription is accepted. The hub reports
  the consumers and their modes to Node, which starts, stops and resizes the phone's video stream
  on demand.
- After the subscription the hub immediately sends its latest frame (live or placeholder), then
  each new frame, scaled to the consumer's size. Each consumer has one pending slot: if a consumer
  is still busy with the previous frame, the newer frame replaces the pending one (latest frame
  wins).

### 3.1 Subscription (consumer → hub)

Right after connecting, the client writes one 24-byte little-endian request. The hub waits at
most 2 s for it.

| Offset | Size | Field     | Value                                                  |
|-------:|-----:|-----------|--------------------------------------------------------|
| 0      | 4    | `magic`   | bytes `M W B S` (`0x5342574D` as a little-endian u32)  |
| 4      | 2    | `version` | 2                                                      |
| 6      | 2    | `size`    | 24                                                     |
| 8      | 4    | `width`   | pixels                                                 |
| 12     | 4    | `height`  | pixels                                                 |
| 16     | 4    | `fpsNum`  | frame rate numerator                                   |
| 20     | 4    | `fpsDen`  | frame rate denominator (1)                             |

The hub disconnects the client when the request is late, malformed, or names a mode that is not
advertised (§1). The client must not write anything else: any further byte disconnects it. A
consumer that wants another mode disconnects and subscribes again.

### 3.2 Frame message (hub → consumer)

Each message is a 48-byte little-endian header followed by `payloadSize` bytes of NV12.

| Offset | Size | Field              | Value                                                        |
|-------:|-----:|--------------------|--------------------------------------------------------------|
| 0      | 4    | `magic`            | bytes `M W B V` (`0x5642574D` as a little-endian u32)        |
| 4      | 2    | `version`          | 2                                                            |
| 6      | 2    | `headerSize`       | 48                                                           |
| 8      | 4    | `width`            | pixels — always the subscribed width                        |
| 12     | 4    | `height`           | pixels — always the subscribed height                       |
| 16     | 4    | `fourcc`           | bytes `N V 1 2` (`0x3231564E`)                               |
| 20     | 4    | `stride`           | bytes per luma row; requires `stride == width`               |
| 24     | 4    | `payloadSize`      | `stride × height × 3 / 2`                                    |
| 28     | 4    | `flags`            | bit 0 `PLACEHOLDER`, bit 1 `FORMAT_CHANGED`; others 0        |
| 32     | 8    | `seq`              | u64, increments per frame produced by the hub                |
| 40     | 8    | `producerQpc100ns` | u64, QueryPerformanceCounter converted to 100 ns units       |

- Pixel format: NV12, BT.709, limited range (Y 16–235, UV 16–240).
- When the ingest size differs from the subscribed size, the hub scales the frame (bilinear) and
  letterboxes it when the aspect ratios differ (black bars: Y 16, UV 128).
- `PLACEHOLDER` marks frames the hub generated (no live video). `FORMAT_CHANGED` is set on the
  first frame sent to a newly subscribed consumer.
- `producerQpc100ns` shares the QPC time base with `MFGetSystemTime()`; consumers may use it for
  latency diagnostics only. Consumers timestamp samples with their own clock.

### 3.3 Consumer validation (mandatory)

Camera components run inside privileged or third-party processes, so every header is validated
before any payload byte is trusted. A consumer must disconnect (and retry later) when:

- `magic`, `version`, `headerSize` or `fourcc` differ from the values above;
- `width`/`height` differ from the mode it subscribed to;
- `stride != width` or `payloadSize != stride × height × 3 / 2`;
- any unknown flag bit is set.

## 4. Timing

Consumers are the frame clock: they deliver samples at the negotiated frame rate and repeat the
last frame when no new frame arrived in time. The hub never paces frames. The phone streams at
the highest frame rate any consumer asked for.
