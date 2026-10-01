# Mobile Webcam Bridge wire protocol — version 1

This protocol runs over a single TCP stream between the **host** (Node.js on Windows) and the
**device** (the Mobile Webcam companion app, on iOS or Android). The device listens on
`127.0.0.1:27100`; the host reaches that port over USB through:

- **iPhone**: usbmuxd (`Connect` with `PortNumber = htons(27100)`), provided by Apple Mobile
  Device Support.
- **Android**: the ADB server (`host:transport:<serial>`, then `tcp:27100`), which requires USB
  debugging to be enabled and the computer to be authorised on the phone.

The protocol itself is identical on both platforms.

All multi-byte integers in the packet header and in binary payloads are **big-endian**, except
PCM audio samples, which are **little-endian** signed 16-bit (`s16le`) as noted below.

## 1. Packet framing

Every packet is a fixed 24-byte header followed by `payloadLength` payload bytes.

| Offset | Size | Field           | Notes                                                                 |
|-------:|-----:|-----------------|-----------------------------------------------------------------------|
| 0      | 4    | `magic`         | `0x4D574252` (ASCII `MWBR`)                                           |
| 4      | 1    | `type`          | Packet type, see §3                                                   |
| 5      | 1    | `flags`         | Meaning depends on `type`; senders set unknown bits to 0              |
| 6      | 2    | `reserved`      | Senders write 0; receivers ignore                                     |
| 8      | 4    | `seq`           | Per-type counter, starts at 0, +1 per packet of that type, wraps 2^32 |
| 12     | 4    | `payloadLength` | Payload size in bytes                                                 |
| 16     | 8    | `timestampUs`   | Signed 64-bit, microseconds on the **sender's** monotonic clock       |

Because `seq` counts per type, a gap in `seq` of `VideoAccessUnit` or `AudioChunk` packets tells
the receiver how many packets the sender dropped before transmission.

### 1.1 Limits

| Payload kind                         | Maximum `payloadLength` |
|--------------------------------------|------------------------:|
| JSON payloads                        | 65 536                  |
| `AudioChunk`                         | 65 536                  |
| `VideoAccessUnit`                    | 4 194 304               |
| `Pong`                               | exactly 16              |
| Types with an empty payload          | exactly 0               |
| Unknown types                        | 4 194 304               |

A receiver that sees a bad magic or a length outside these limits must treat it as a fatal
protocol error and close the connection. There is no resynchronisation.

### 1.2 Forward compatibility

- The header layout is frozen for every 1.x version.
- Receivers skip packets of unknown `type` (after validating the length limit).
- Receivers ignore unknown JSON keys. New fields are only ever added.

## 2. Session lifecycle

1. On connect, **both sides send `Hello` immediately**, without waiting for the peer.
2. Each side validates the peer's `protocol.major`. On mismatch it sends
   `Error {code: "VERSION_MISMATCH", fatal: true}` and closes. The effective minor version is
   `min(local.minor, peer.minor)`.
3. The device ignores every command received before the host's `Hello`.
4. Both sides send `Ping` every second once handshaken. Any inbound packet counts as liveness.
   A side that receives nothing for 4 seconds closes the connection.
5. The device accepts one host connection at a time. A new connection replaces the old one.

## 3. Packet types

| Type   | Name              | Direction      | Payload                                   | Flags                                   |
|--------|-------------------|----------------|-------------------------------------------|-----------------------------------------|
| `0x01` | `Hello`           | both           | JSON `Hello`                              | —                                       |
| `0x02` | `Error`           | both           | JSON `Error`                              | —                                       |
| `0x03` | `Ping`            | both           | empty                                     | —                                       |
| `0x04` | `Pong`            | both           | 16 bytes: `echoT0` i64, `t1` i64          | —                                       |
| `0x10` | `StartVideo`      | host → device  | JSON `StartVideo`                         | —                                       |
| `0x11` | `StopVideo`       | host → device  | empty                                     | —                                       |
| `0x12` | `RequestKeyframe` | host → device  | empty                                     | —                                       |
| `0x13` | `StartAudio`      | host → device  | JSON `StartAudio`                         | —                                       |
| `0x14` | `StopAudio`       | host → device  | empty                                     | —                                       |
| `0x20` | `VideoConfig`     | device → host  | JSON `VideoConfig`                        | —                                       |
| `0x21` | `VideoAccessUnit` | device → host  | H.264 Annex-B access unit                 | `0x01` IDR, `0x02` PARAM_SETS, `0x04` DISPOSABLE |
| `0x30` | `AudioConfig`     | device → host  | JSON `AudioConfig`                        | —                                       |
| `0x31` | `AudioChunk`      | device → host  | PCM `s16le`, interleaved                  | `0x01` DISCONTINUITY                    |
| `0x40` | `Status`          | device → host  | JSON `Status`                             | —                                       |
| `0x41` | `Log`             | device → host  | JSON `Log` (≤ 8 192 bytes)                | —                                       |

### 3.1 Timing packets

- `Ping`: `timestampUs` is the sender's clock when sending (`t0`).
- `Pong`: payload carries `echoT0` (the `t0` from the `Ping`) and `t1` (responder's clock when
  the `Ping` arrived). The `Pong` header's `timestampUs` is the responder's clock when sending
  (`t2`). The original sender records `t3` on arrival and computes, NTP style:
  - `rtt = (t3 − t0) − (t2 − t1)`
  - `offset = ((t1 − t0) + (t2 − t3)) / 2` (peer clock minus local clock)
  - Receivers keep the sample with the lowest RTT among the last 16.

### 3.2 Media timestamps

- `VideoAccessUnit.timestampUs`: capture presentation time converted to the device monotonic
  clock (iOS: `CMClockGetHostTimeClock`; Android: `SystemClock.elapsedRealtimeNanos` /
  `CLOCK_BOOTTIME`, the time base of camera sensor timestamps).
- `AudioChunk.timestampUs`: capture time of the chunk's first sample, same clock.
- The host maps device time to host time with the offset from §3.1.

### 3.3 Video rules

- The device sends `VideoConfig` before the first access unit of every configuration, and again
  whenever any field changes (`configId` increments).
- The first access unit after a `VideoConfig` is an IDR carrying SPS/PPS (`flags = 0x03`).
- Every access unit is a complete Annex-B access unit (4-byte start codes), without an access
  unit delimiter. The host may append its own AUD.
- `RequestKeyframe`: the device produces an IDR with SPS/PPS as soon as possible, rate-limited to
  2 per second.
- `StartVideo` is idempotent: if video is already running with the same parameters it does
  nothing; if parameters differ the device reconfigures and sends a new `VideoConfig`.

### 3.4 Audio rules

- The device sends `AudioConfig` before the first chunk of every configuration.
- v1 always uses `s16le`, 48 000 Hz, 1 channel. Chunks carry 10–40 ms of audio.
- `DISCONTINUITY` marks the first chunk after a gap (interruption, route change, restart).
- The device never drops audio deliberately. If more than 1 s of audio is queued for sending,
  it closes the connection.

## 4. JSON payloads

All JSON payloads are UTF-8 **canonical JSON**: object keys sorted by Unicode code point, no
insignificant whitespace, no escaped forward slashes, and **integers only** (no floating-point
numbers anywhere in v1). Canonical form lets the golden test vectors match byte for byte across
implementations. Optional fields are omitted rather than set to `null`.

### `Hello`

```json
{"app":{"build":"42","gitSha":"0123abc","name":"mobile-webcam-bridge-ios","version":"0.1.0"},"device":{"model":"iPhone16,1","name":"iPhone","os":"iOS 26.0"},"features":["audio.pcm","log","status","video.h264"],"protocol":{"major":1,"minor":0},"role":"device"}
```

| Field       | Type                       | Notes                                          |
|-------------|----------------------------|------------------------------------------------|
| `protocol`  | `{major, minor}`           | v1: `{1, 0}`                                   |
| `role`      | `"host"` \| `"device"`     |                                                |
| `app`       | `{name, version, build, gitSha}` | strings                                  |
| `device`    | `{model, name, os}`        | device role only, e.g. `iPhone16,1` / `iOS 26.0` or `Pixel 9` / `Android 16` |
| `features`  | string[]                   | sorted; informative                            |

### `Error`

```json
{"code":"CAMERA_UNAVAILABLE","fatal":false,"message":"Camera is in use by another app"}
```

`code` ∈ `VERSION_MISMATCH`, `BAD_REQUEST`, `PERMISSION_DENIED`, `CAMERA_UNAVAILABLE`,
`MIC_UNAVAILABLE`, `ENCODER_FAILURE`, `INTERNAL`. `fatal: true` means the sender closes the
connection after sending.

### `StartVideo`

```json
{"bitrateKbps":6000,"camera":"back.wide","encoder":"lowLatency","fps":30,"height":720,"mirror":false,"orientation":"auto","width":1280}
```

| Field         | Values                                                              |
|---------------|---------------------------------------------------------------------|
| `width`, `height` | output frame size in landscape terms (e.g. 1280×720, 1920×1080) |
| `fps`         | 15–60                                                               |
| `bitrateKbps` | 500–20 000                                                          |
| `camera`      | `back.wide`, `back.ultraWide`, `back.telephoto`, `front`. A device without the requested lens falls back to `back.wide` and reports the camera actually used in `VideoConfig.camera` |
| `mirror`      | boolean                                                             |
| `orientation` | `auto` (follow device), `landscape`, `portrait`                     |
| `encoder`     | `lowLatency` (iOS: VideoToolbox low-latency rate control; Android: MediaCodec realtime priority, `KEY_LATENCY` 1, and `KEY_LOW_LATENCY` where the codec supports it) or `standard` |

### `StartAudio`

```json
{"processing":"standard"}
```

`processing` selects the platform audio path:

| Value      | iOS (AVAudioSession mode) | Android (`AudioRecord` source)                         |
|------------|---------------------------|--------------------------------------------------------|
| `standard` | `.videoRecording`         | `CAMCORDER`                                            |
| `raw`      | `.measurement`            | `UNPROCESSED` when supported, else `VOICE_RECOGNITION` |
| `voice`    | `.voiceChat`              | `VOICE_COMMUNICATION`                                  |

### `VideoConfig`

```json
{"bitrateKbps":6000,"camera":"back.wide","codec":"h264","configId":1,"encoder":"lowLatency","fps":30,"height":720,"mirrored":false,"profile":"constrainedHigh","rotationDeg":0,"width":1280}
```

`width`/`height` are the dimensions of the encoded frames (portrait orientation swaps them).

### `AudioConfig`

```json
{"channels":1,"configId":1,"format":"s16le","sampleRate":48000}
```

### `Status`

Full snapshot, sent whenever anything changes and at least every 5 s.

```json
{"appState":"active","audio":{"state":"running"},"battery":{"charging":true,"levelPercent":81},"lowPower":false,"permissions":{"camera":"authorized","microphone":"authorized"},"thermal":"nominal","video":{"bitrateKbps":6000,"fps":30,"height":720,"state":"running","width":1280}}
```

| Field         | Values                                                                          |
|---------------|---------------------------------------------------------------------------------|
| `video.state` | `off`, `starting`, `running`, `interrupted`, `error`                            |
| `video.reason`| optional string, e.g. `background`, `inUseByAnotherClient`, `thermal`           |
| `audio.state` | `off`, `starting`, `running`, `interrupted`, `error` (+ optional `reason`)      |
| `thermal`     | `nominal`, `fair`, `serious`, `critical`                                        |
| `battery`     | `{levelPercent 0–100, charging}` (omitted if unknown)                           |
| `permissions` | `camera` / `microphone` ∈ `authorized`, `denied`, `restricted`, `notDetermined` |
| `appState`    | `active`, `inactive`, `background`                                              |

### `Log`

```json
{"category":"capture","level":"info","message":"Session started"}
```

`level` ∈ `debug`, `info`, `warn`, `error`. The header `timestampUs` is the event time.

## 5. Test vectors

`test-vectors/packets.json` holds valid packets (hex + decoded form), invalid streams with the
expected error code, and multi-packet streams. `test-vectors/h264.json` covers Annex-B handling.
`test-vectors/usbmux.json` covers the usbmux framing used by the host. Every implementation
(TypeScript host, Swift `BridgeKit`, Kotlin `android/protocol`) runs its codec against these files.
