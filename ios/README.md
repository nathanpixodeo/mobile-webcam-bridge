# Mobile Webcam — iOS companion app

The app captures the camera (H.264, VideoToolbox) and the microphone (PCM, 48 kHz mono) and
serves them on `127.0.0.1:27100` on the phone. The Windows host reaches that port through the
USB cable (usbmux) — nothing is exposed on Wi-Fi. The wire protocol is specified in
[`protocol/SPEC.md`](../protocol/SPEC.md).

```
ios/
  project.yml        XcodeGen spec (the .xcodeproj is generated, not committed)
  BridgeKit/         Swift package, Foundation only: protocol, Annex-B, scheduling, session logic
  MobileWebcam/      App target: capture, encoder, Network transport, SwiftUI
```

`BridgeKit` builds and tests on Linux (and with the Swift toolchain for Windows):

```sh
swift test --package-path ios/BridgeKit
```

Its tests run against the golden vectors in `protocol/test-vectors/`, the same files the host
tests use, so both sides agree byte for byte.

## Building without a Mac

The `ios` GitHub Actions workflow (`.github/workflows/ios.yml`) does everything:

1. `swift test` for BridgeKit on Linux (cheap minutes).
2. On a `macos-26` runner: XcodeGen → `xcodebuild archive` without code signing → `MobileWebcam.ipa`,
   uploaded as the artifact `mobile-webcam-ipa`.

It runs on every push that touches `ios/` or `protocol/`, or manually (**Actions → ios → Run
workflow**). macOS minutes cost about ten times Linux minutes; a build takes 6–10 minutes, so a
private repository on the free plan allows roughly 20–30 builds a month (public repositories are
free).

Download the latest IPA from Windows:

```sh
gh run download --name mobile-webcam-ipa --dir build/ios
```

## Installing with Sideloadly (Windows)

1. Install **iTunes and iCloud from apple.com** (the Microsoft Store versions do not work with
   Sideloadly), then install [Sideloadly](https://sideloadly.io/).
2. Connect the iPhone by USB, unlock it and tap **Trust This Computer**.
3. Drag `MobileWebcam.ipa` into Sideloadly, enter an Apple ID and press **Start**. A secondary
   Apple ID is a good idea: the credentials are handed to a third-party tool.
4. On the iPhone, enable **Settings › Privacy & Security › Developer Mode** (iOS 16+) and restart.
5. Trust the signing profile in **Settings › General › VPN & Device Management**.

Free Apple IDs sign apps for **7 days**, allow 3 sideloaded apps at a time and 10 new App IDs
per week. Keep the bundle identifier (`dev.mobilewebcambridge.ios`) stable between installs so
camera and microphone permissions survive re-signing. Sideloadly can refresh the app
automatically while the PC is on.

## Using it

Open the app and keep it in the foreground: iOS stops camera capture for apps in the background.
The microphone keeps running when the screen locks (`audio` background mode). **Standby**
blacks out the screen to save power during long calls; tap to wake.

The app does nothing until the host connects; the host then starts video when a Windows app
opens the camera, and audio when one opens the microphone. Rotation follows the phone unless the
host requests a fixed orientation. Under thermal pressure the frame rate and bitrate drop
(serious: 24 fps, 70 %; critical: 15 fps, 50 %) and video stops at the shutdown level.

## Logs and diagnostics

There is no Xcode debugger in this setup, so the app sends its log to the host after every
handshake (at most 100 lines per second): the host prints them with `component: "ios"`. Each
connection also starts with a boot report (model, iOS version, build, git commit, camera formats)
and any crash or hang reports MetricKit delivered since the last connection.

On the phone, **Diagnostics** shows the build, round-trip time, video rate, dropped frames and the
newest log lines. Its **Test pattern and tone** switch replaces the camera and microphone with
synthetic media, which tells capture problems from transport problems.

As a fallback, the device log is available over USB with libimobiledevice:

```sh
idevicesyslog --process MobileWebcam
idevicecrashreport -e ./crashes
```

## Design notes

- **Threads.** One serial queue each for transport (Network callbacks, the session reducer, the
  send scheduler), video (capture and encoding) and audio; UI on the main actor. Classes that own
  a queue are `@unchecked Sendable` with all mutable state confined to that queue. Pixel and sample
  buffers never leave their queue; only Sendable values (`EncodedVideoFrame`, `AudioChunk`) cross
  over, via `queue.async`, which keeps FIFO order without a Task per frame.
- **Back-pressure.** The send scheduler orders control > audio > video > log, keeps at most
  256 KiB in flight, sheds disposable frames first and then everything up to the next IDR (and
  asks the encoder for one). Audio is never dropped.
- **Encoder.** Low-latency rate control (infinite GOP, keyframes on request), falling back to the
  standard encoder (2 s GOP) when unavailable. Every size, rotation or rate change creates a new
  encoder with a new `configId`; frames from an older configuration are discarded.
