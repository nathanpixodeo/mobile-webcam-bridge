# Architecture

## 1. Why this shape

Three platform facts dictate the design:

- **iOS does not expose its camera or microphone over USB** (no UVC class, no lockdown service).
  A companion app on the iPhone must capture, encode and stream. It listens on the device's
  loopback (`127.0.0.1:27100`); the PC reaches it through usbmuxd's `Connect`, which turns a TCP
  connection to `127.0.0.1:27015` (Apple Mobile Device Service) into a byte tunnel to that port.
- **Android needs a companion app too.** Some Android 14+ devices offer a native "USB webcam" mode,
  but it is video only and available on few devices. The app works on any Android 10+ phone and
  adds the microphone and remote control. It listens on the phone's loopback (`127.0.0.1:27100`);
  the PC reaches it through the local ADB server (`127.0.0.1:5037`): `host:transport:<serial>`
  followed by `tcp:27100` opens a byte tunnel to that port, the same idea as usbmux `Connect`.
  USB debugging must be enabled on the phone.
- **Windows user-mode code cannot create camera or microphone devices.** A virtual camera needs a
  COM media source loaded by the Windows Camera Frame Server (Windows 11, `MFCreateVirtualCamera`)
  or a DirectShow filter loaded into each app (Windows 10). A virtual microphone needs a kernel
  audio driver.

Both phone platforms speak the same wire protocol; only the tunnel differs. Node.js is the
orchestrator; native code is kept to what Windows requires and is deliberately "dumb".

## 2. Components and data flow

```
Phone ─ Mobile Webcam app
  iOS:      AVCaptureSession ─► VTCompressionSession (H.264, low-latency rate control, no B-frames)
            AVCaptureAudioDataOutput ─► PCM s16le 48 kHz mono
  Android:  Camera2 ─► MediaCodec (H.264, low-latency)
            AudioRecord ─► PCM s16le 48 kHz mono
  loopback server 127.0.0.1:27100 (iOS: NWListener) ── wire protocol v1 (protocol/SPEC.md)
         │ USB: usbmux Connect (iPhone)  /  ADB host:transport:<serial> + tcp:27100 (Android)
Windows  ▼
  Node host (host/)
    UsbmuxDeviceWatcher ─┐
    AdbDeviceWatcher ────┴─► CompositeDeviceWatcher ─► DeviceManager ─► DeviceSession ─► PeerConnection
                             (CompositeTunnelFactory opens the usbmux or the ADB tunnel for each device)
    VideoPipeline:  access units ─► KeyframeGate ─► ffmpeg (stdin) ──raw NV12──► ingest pipe
    AudioPipeline:  PCM ─► LinearResampler(ratio from AudioDriftCompensator) ─► mic feeder stdin
  bridge-native.exe video hub
    ingest pipe \\.\pipe\mobile-webcam-bridge-ingest-<token> (private) ─► public pipe \\.\pipe\mobile-webcam-bridge-video ─► consumers
  vcam-mf.dll (in Frame Server)  /  vcam-dshow.dll (in each app)  ─► "Mobile Webcam Windows Virtual Camera" (Windows 11) / "Mobile Webcam" (Windows 10)
  bridge-native.exe mic feed ─IOCTL_MWBMIC_WRITE─► \\.\MobileWebcamBridgeMic ─► mwbmic.sys ─► "Mobile Webcam Microphone"
```

Frame bytes never pass through Node: ffmpeg writes NV12 straight into the hub's ingest pipe.
Node moves only compressed video (a few Mbit/s) and 96 KB/s of PCM.

## 3. Design principles

1. **Dumb privileged components, smart user mode.** The camera DLLs run inside a LocalService
   process or third-party apps; the driver runs in the kernel. They accept only raw frames/PCM of
   a fixed format and validate every header. Decoding device data, retries, drift compensation
   and policy live in user mode.
2. **Demand-driven streaming.** The hub reports how many apps read the camera; the driver reports
   whether an app captures from the microphone. The host starts the matching phone stream on the
   first consumer and stops it after a grace period (3 s video, 2 s audio) once the last leaves.
3. **Level-triggered reconciliation.** `StreamReconciler` compares desired streams with what was
   last requested and emits only the difference; after a reconnect it replays everything.
4. **Ports and adapters.** `domain/` is pure (no I/O, deterministic tests), `ports/` declares
   interfaces, `infrastructure/` implements them, `application/` orchestrates, and
   `composition/createBridgeApp.ts` is the only place that wires concrete classes. Constructor
   injection everywhere; time comes from an injected `Clock`.
5. **One spec, several implementations.** Every cross-language boundary has a spec in
   `protocol/` and golden vectors consumed by the TypeScript, Swift and Kotlin test suites.

## 4. Host layers

| Layer | Contents |
|-------|----------|
| `domain/` | Packet codec and incremental reader, canonical JSON, message schemas, Annex-B helpers, keyframe gate, reconnect policy, backoff, clock offset estimation, stream reconciler, demand tracker, PI controller, drift compensator, resampler, placeholder policy, error hierarchy, typed events |
| `ports/` | `DeviceWatcher` (with the `MobileDevice` union — `UsbmuxDevice`, `AdbDevice`, `TcpDevice`, each with `platform`, `link`, `model` — and `PendingDevice` for a phone that is connected but needs user action), `DeviceTunnelFactory`, `VideoOutput`, `VideoDecoder(Factory)`, `AudioSink`, `NativeHelper`, `Logger`, `Metrics` |
| `application/` | `BridgeService`, `DeviceManager`, `DeviceSession`, `PeerConnection`, `StreamCoordinator`, `VideoPipeline`, `AudioPipeline`, `MetricsReporter`, `ProbeRecorder`, `Doctor` |
| `infrastructure/` | `usbmux/` (client, watcher, tunnels), `adb/` (`AdbClient`, `AdbConnection`, `AdbDeviceWatcher`, `AdbTunnelFactory`, `AdbServerLauncher`), `tunnel/` (`CompositeDeviceWatcher`, `CompositeTunnelFactory`, `socketTunnel`), TCP tunnels (dev/test), ffmpeg decoder and placeholder renderer, bridge-native adapters (CLI, video hub, mic sink), pino logging, zod config, metrics, system clock |
| `cli/` | One class per command, `CliApplication` dispatcher |

### Device transports

- **iPhone (usbmux).** `UsbmuxDeviceWatcher` and `UsbmuxTunnelFactory` talk to Apple Mobile Device
  Service (`usbmux.address`, default `127.0.0.1:27015`).
- **Android (ADB).** `AdbDeviceWatcher` follows `host:track-devices-l` on the ADB server
  (`adb.address`, default `127.0.0.1:5037`); `AdbTunnelFactory` opens `host:transport:<serial>`
  followed by `tcp:27100`. A phone that is `unauthorized` or `offline` is reported as a
  `PendingDevice` until the user acts. `AdbServerLauncher` runs `adb start-server` when the server
  is down and `adb` is found (`adb.path`, `PATH`, `%LOCALAPPDATA%\Android\Sdk\platform-tools`; turn
  it off with `adb.startServer`). While the server is unreachable the watcher reports
  `ADB_UNAVAILABLE` and keeps retrying.
- **Composition.** `CompositeDeviceWatcher` merges the enabled watchers (`usbmux.enabled`,
  `adb.enabled`); an unavailable service never hides the other transport's phones.
  `CompositeTunnelFactory` routes each `MobileDevice` to the tunnel factory of its transport and
  `socketTunnel` hands the connected socket to the session as a byte tunnel. Everything above the
  tunnel is platform-independent.
- With several phones attached the first USB phone is used; `device.id` or `--device` pins one.
  Phones connected by wireless debugging are ignored unless `adb.includeNetworkDevices` is set.
  `--tcp <host>` (development) replaces all of this with a single `TcpDevice`.

### Session lifecycle

`idle → connecting → handshaking → ready`, and on failure `backoff → connecting` or `stalled`:

- `APP_NOT_REACHABLE` (the app is not open, so nothing listens on the phone's port): poll every
  second, quietly. usbmux refuses the connection (`ConnectionRefused`); ADB accepts the stream and
  closes it, so a tunnel that closes before the app's `Hello` counts the same way.
- Device gone: wait for the watcher to report it again.
- Protocol major mismatch: stall until re-attach.
- Anything else: exponential backoff (250 ms ×2, max 5 s, ±20 % jitter), reset after 10 s of a
  stable connection.
- Heartbeat: both sides ping every second; 4 s of silence ends the connection.

### Video path

- The device sends `VideoConfig`, then an IDR with SPS/PPS, then access units.
- `KeyframeGate` drops access units until an IDR after any loss (start, decoder congestion,
  reconnect), and the pipeline requests a keyframe (rate-limited to one per 500 ms).
- The camera advertises a catalog of modes (640×360 to 3840×2160, 15/30/60 fps, 4K up to 30 fps;
  protocol/FRAME_PIPE.md §1) and every application picks one. Each consumer subscribes to its
  mode on the hub's pipe; the hub reports the consumers' modes to Node.
- `ModeArbiter` streams the largest requested size at the highest requested frame rate: it
  upgrades immediately and downgrades only after `video.modeDowngradeGraceMs` (3 s). A change
  re-sends `StartVideo` to the phone, restarts ffmpeg for the new output size and switches the
  hub's ingest size (`ingest` → `ingestMode`). Meanwhile the hub keeps serving every consumer by
  scaling the previous frames, so a switch shows no black frames.
- The bitrate follows the mode: `W × H × fps × video.bitsPerPixel` (0.1), clamped to
  1.5–40 Mbps (about 6 Mbps at 1080p30, 25 Mbps at 4K30), unless `video.bitrateKbps` overrides it.
- ffmpeg decodes with `-flags low_delay`, a tiny probe, `-fps_mode passthrough`, and scales/pads
  into the streamed mode. Above 1080p it decodes with `d3d11va` (`video.hwaccel: auto`) and falls
  back to software if that fails. After each access unit the host writes an access unit delimiter
  so ffmpeg's parser emits the frame immediately (measured: ~47 ms decode latency at 30 fps
  instead of ~94 ms). `-fflags nobuffer` must not be used: it drops frames with raw H.264.
- The hub scales frames per consumer (2×2 box passes for large reductions, then bilinear) and
  letterboxes when the aspect ratios differ (e.g. a 640×480 consumer of a 16:9 stream).
- A crashed decoder restarts at most three times per minute.

### Audio path

- The driver consumes at its own clock; the phone produces at another. The mic feeder reports
  the driver's buffer fill every 100 ms. `AudioDriftCompensator` (PI controller, smoothing, slew
  limit, ±1000 ppm clamp) steers the resampling ratio so the fill stays near 40 ms; underruns and
  large overshoots are fixed by inserting silence or dropping input.

## 5. Native components

| Binary | Runs in | Responsibilities |
|--------|---------|------------------|
| `bridge-native.exe` | user session (self-elevates for install) | install/uninstall with rollback, status, doctor, video hub, mic feeder, diagnostics |
| `vcam-mf.dll` | Frame Server (LocalService) and FrameServerMonitor | IMFMediaSourceEx/IMFMediaStream2; NV12 + YUY2; paced delivery; pipe client (SQOS anonymous) |
| `vcam-dshow.dll` | each DirectShow app (x86 and x64) | CSource/CSourceStream; YUY2, NV12, I420, RGB24 |
| `mwbmic.sys` | kernel | PortCls WaveRT capture endpoint; control device for the feed; 64 KiB ring |

Security notes:

- The public video pipe denies network logons, rejects remote clients, grants read to
  LocalService and full access only to SYSTEM, Administrators and the hub's user.
- Pipe clients open with `SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS` so a squatting server cannot
  impersonate a privileged consumer.
- The driver's control device allows interactive users, admits one handle at a time and resets
  its buffer when that handle closes.
- Elevation results travel over a random-named pipe, never through a file path chosen by a
  non-elevated process.

## 6. Phone apps

### iOS app

`BridgeKit` (Foundation-only Swift package, tested on Linux CI) holds the protocol codec,
canonical JSON, Annex-B conversion, the send scheduler (control > audio > video > log; drop
disposable frames first, then drop until the next IDR; never drop audio), the session reducer,
heartbeat, clock offset, thermal policy and log ring. The app target adds capture, VideoToolbox
encoding, the loopback server, and system monitors. See `ios/README.md`.

### Android app

Kotlin, in two Gradle modules. `android/protocol` is pure Kotlin: the packet codec, canonical JSON,
the send scheduler and the session reducer, tested against `protocol/test-vectors`. `android/app`
adds Camera2 capture with MediaCodec H.264 (low-latency), `AudioRecord` (48 kHz mono PCM16), a
loopback server on `127.0.0.1:27100` and the Compose UI. Streaming runs in a foreground service
with the camera and microphone types, so it continues with the screen off. See `android/README.md`.

## 7. Known limits

- Decode latency is about one frame interval plus ~14 ms with ffmpeg; a Media Foundation decoder
  could remove the remaining frame of delay.
- Frame Server shares one stream of the Media Foundation camera between all applications, so
  on Windows 11 the first application's mode is what everybody gets; per-application modes
  apply to DirectShow (Windows 10) consumers.
- 3840×2160 is offered up to 30 fps; raw 4K60 would move ~750 MB/s through each consumer.
- The iOS camera stops when the app is in the background (iOS restriction); the microphone keeps
  running with the screen locked.
- The Android app streams from a foreground service, so camera and microphone keep running with
  the screen off; aggressive battery optimisation can still stop it (exclude the app).
- ARM64 Windows and multiple simultaneous phones are out of scope for v1.
