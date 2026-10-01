# Mobile Webcam — Android companion app

The app captures the camera (H.264, MediaCodec) and the microphone (PCM, 48 kHz mono) and serves
them on `127.0.0.1:27100` on the phone. The Windows host reaches that port through the USB cable
with ADB (`host:transport:<serial>`, then `tcp:27100`) — nothing is exposed on Wi-Fi. It speaks
the same wire protocol as the iOS app, specified in [`protocol/SPEC.md`](../protocol/SPEC.md).

```
android/
  gradle/libs.versions.toml   Versions (AGP 9.4, Kotlin 2.4, Compose BOM, JUnit 5)
  protocol/                   Kotlin/JVM library, no Android imports: wire codec, canonical JSON,
                              messages, Annex-B, send scheduler, session reducer, policies
  app/                        Android app: Camera2 + OpenGL + MediaCodec, AudioRecord, TCP server,
                              foreground service, Compose UI
```

Requirements: Android 10 (API 29) or later, USB debugging. Package name
`dev.mobilewebcambridge.android`, app name **Mobile Webcam**.

## Building

GitHub Actions builds the APK on every push that touches `android/` or `protocol/` (or manually:
**Actions → android → Run workflow**), on a Linux runner:

```sh
./gradlew :protocol:test :app:testDebugUnitTest :app:assembleDebug
```

The `protocol` tests run against the golden vectors in `protocol/test-vectors/`, the same files
the host and BridgeKit tests use, so all implementations agree byte for byte (including streams
split at every byte and seeded random splits).

Locally, the same command works from `android/` with JDK 17 or later and the Android SDK
(`ANDROID_HOME`, or `sdk.dir` in `local.properties`); Android Studio opens the folder as is.

## Installing

1. On the phone: **Settings › About phone**, tap **Build number** seven times, then turn on
   **Developer options › USB debugging** (menu names vary by manufacturer).
2. On Windows: `winget install Google.PlatformTools`, connect the phone, unlock it and accept
   **Allow USB debugging?**. `adb devices -l` lists the phone as `device`.
3. Download and install the latest build:

   ```sh
   gh run download --name mobile-webcam-apk --dir build/android
   adb install -r build/android/app-debug.apk
   ```

The workflow reuses one debug signing key (kept in the Actions cache), so `adb install -r`
updates the app in place. If the cache was evicted (no build for 7 days), the signature changes
and the install fails with `INSTALL_FAILED_UPDATE_INCOMPATIBLE`: run
`adb uninstall dev.mobilewebcambridge.android` once, install again and re-grant the permissions.

## Using it

Open **Mobile Webcam** and allow the camera and the microphone. The app starts a foreground
service (notification "Mobile Webcam is ready") that listens for the host; streaming continues
with the screen off or another app in front, until **Stop** (in the app or the notification).
**Standby** blacks out the screen at minimum brightness for long calls; tap to wake.

The app does nothing until the host connects; the host then starts video when a Windows app opens
the camera, and audio when one opens the microphone. Rotation follows the phone unless the host
requests a fixed orientation. Under thermal pressure the frame rate and bitrate drop (Android
thermal status moderate: 24 fps, 70 %; severe: 15 fps, 50 %) and video stops from critical on.
Some manufacturers kill foreground services aggressively: exclude Mobile Webcam from battery
optimisation if the stream stops after a while.

## Logs and diagnostics

The app sends its log to the host after every handshake (at most 100 lines per second); the host
prints them with `component: "android"`. Each connection starts with a boot report (model,
Android version, build, git commit, cameras, H.264 encoders) and, on Android 11+, the reasons the
previous runs ended (crash, ANR, low-memory kill) from `ApplicationExitInfo`.

On the phone, the **Diagnostics** card shows the round-trip time, the video rate and dropped
frames, and the **Log** card the newest lines. The **Test pattern** switch replaces the camera and
microphone with colour bars (with a frame counter) and a 440 Hz tone, which tells capture problems
from transport problems. Over USB:

```sh
adb logcat -s MobileWebcam
```

## Design notes

- **Threads.** One `HandlerThread` each for the session (connection events, reducer, send
  scheduler, 100 ms tick), video, audio, the camera, OpenGL rendering and the encoder; a blocking
  reader and writer thread per connection; the microphone reads on its own thread. Components
  keep their mutable state on one thread and hand immutable values over with `post`, which keeps
  FIFO order without locks.
- **Video.** Camera2 renders into a `SurfaceTexture`; OpenGL draws each frame into the encoder's
  input surface, rotated to the phone's pose, centre-cropped to the requested size, optionally
  mirrored and paced to the requested rate. The transform Camera2 attaches to its buffers is read
  back from the texture matrix and compensated, so the result does not depend on how the camera
  HAL delivers its buffers (`capture/Geometry.kt`, unit-tested). Timestamps are sensor timestamps
  on `CLOCK_BOOTTIME` (`elapsedRealtimeNanos`); audio uses the same clock via `AudioTimestamp`.
- **Cameras.** `back.ultraWide` and `back.telephoto` use a separate back camera when the phone
  exposes one, otherwise the zoom ratio of the logical back camera (Android 11+). A phone without
  the lens falls back to `back.wide` and says so in `VideoConfig.camera`.
- **Encoder.** Hardware H.264 in asynchronous mode: no B-frames, SPS/PPS before every IDR,
  `KEY_LATENCY` 1 and (Android 11+, when supported) `KEY_LOW_LATENCY`, realtime priority, CBR in
  low-latency mode, a 10 s GOP with keyframes on request (2 s in standard mode), BT.709. A
  rotation creates a new encoder and `configId`; a thermal change sets the bitrate live, lowers the
  frame rate in the renderer and starts a new `configId` at the next (requested) IDR.
- **Back-pressure.** Same scheduler as iOS: control > audio > video > log, at most 256 KiB in
  flight, disposable frames shed first, then everything up to the next IDR (and the encoder is
  asked for one). Audio is never dropped; more than one second queued closes the connection.
- **Audio sources.** `standard` → `CAMCORDER`, `raw` → `UNPROCESSED` when the phone supports it
  (else `VOICE_RECOGNITION`), `voice` → `VOICE_COMMUNICATION`. When another app (a call, an
  assistant) takes priority, Android silences the recording; the status shows `interrupted`
  with reason `silenced` until it ends.
