# Frame pipe protocol — version 1

Raw video moves from the decoder to the virtual camera components through two named pipes owned
by `bridge-native.exe video hub` (the **hub**). Frame bytes never pass through Node.

```
ffmpeg ──raw NV12──► ingest pipe ──► hub ──header+NV12──► public pipe ──► vcam-mf.dll / vcam-dshow.dll
```

C/C++ definition: `native/common/include/mwb/FrameProtocol.h`.

## 1. Ingest pipe (decoder → hub)

- Name: `\\.\pipe\mobile-webcam-bridge-ingest-<token>`; Node generates `<token>` (16 random hex chars)
  and passes the full name to the hub with `--ingest-pipe`.
- Created by the hub with a DACL that grants access only to SYSTEM, Administrators and the
  hub's own user SID, plus `PIPE_REJECT_REMOTE_CLIENTS`.
- Content: raw NV12 frames back to back, no headers. One frame = `width × height × 3 / 2` bytes
  for the camera mode the hub was started with. Rows are tightly packed (stride = width).
- One writer at a time. When the writer disconnects, the hub discards any partial frame and
  waits for the next writer (ffmpeg is restarted on every decoder restart).

## 2. Public pipe (hub → camera components)

- Name: `\\.\pipe\mobile-webcam-bridge-video` (override: registry `PipeName`, hub `--pipe-name`).
- Created by the hub with:
  - SDDL `D:P(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;<hubUserSid>)(A;;GR;;;LS)`
    (deny network logons; LocalService — the Frame Server account — may only read),
  - `PIPE_REJECT_REMOTE_CLIENTS`,
  - `FILE_FLAG_FIRST_PIPE_INSTANCE` on the first instance, so a squatted name is an error.
- Byte mode. Clients open with `GENERIC_READ` only and must pass
  `SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS` to `CreateFileW`.
- Every connected client is a **consumer**. The hub reports the consumer count to Node, which
  starts and stops the phone's video stream on demand.
- On connect the hub immediately sends its latest frame (live or placeholder), then each new
  frame. Each consumer has one pending slot: if a consumer is still busy with the previous frame,
  the newer frame replaces the pending one (latest frame wins).

### 2.1 Frame message

Each message is a 48-byte little-endian header followed by `payloadSize` bytes of NV12.

| Offset | Size | Field              | Value                                                        |
|-------:|-----:|--------------------|--------------------------------------------------------------|
| 0      | 4    | `magic`            | bytes `M W B V` (`0x5642574D` as a little-endian u32)        |
| 4      | 2    | `version`          | 1                                                            |
| 6      | 2    | `headerSize`       | 48                                                           |
| 8      | 4    | `width`            | pixels                                                       |
| 12     | 4    | `height`           | pixels                                                       |
| 16     | 4    | `fourcc`           | bytes `N V 1 2` (`0x3231564E`)                               |
| 20     | 4    | `stride`           | bytes per luma row; v1 requires `stride == width`            |
| 24     | 4    | `payloadSize`      | `stride × height × 3 / 2`                                    |
| 28     | 4    | `flags`            | bit 0 `PLACEHOLDER`, bit 1 `FORMAT_CHANGED`; others 0        |
| 32     | 8    | `seq`              | u64, increments per frame produced by the hub                |
| 40     | 8    | `producerQpc100ns` | u64, QueryPerformanceCounter converted to 100 ns units       |

- Pixel format: NV12, BT.709, limited range (Y 16–235, UV 16–240).
- `PLACEHOLDER` marks frames the hub generated (no live video). `FORMAT_CHANGED` is set on the
  first frame sent to a newly connected consumer.
- `producerQpc100ns` shares the QPC time base with `MFGetSystemTime()`; consumers may use it for
  latency diagnostics only. Consumers timestamp samples with their own clock.

### 2.2 Consumer validation (mandatory)

Camera components run inside privileged or third-party processes, so every header is validated
before any payload byte is trusted. A consumer must disconnect (and retry later) when:

- `magic`, `version`, `headerSize` or `fourcc` differ from the values above;
- `width`/`height` differ from the installed camera mode (registry);
- `stride != width` or `payloadSize != stride × height × 3 / 2`;
- any unknown flag bit is set.

## 3. Timing

Consumers are the frame clock: they deliver samples at the negotiated frame rate and repeat the
last frame when no new frame arrived in time. The hub never paces frames.
