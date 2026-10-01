# Mobile Webcam Bridge

Use an iPhone or an Android phone as a **webcam and microphone on Windows over USB**. Any Windows
app — Zoom, Teams, Chrome/Edge, OBS, Discord, the Windows Camera app — sees a *Mobile Webcam*
camera and a *Mobile Webcam Microphone*.

```
iPhone app (Swift)   ──USB/usbmux──┐
                                   ├─► Node host (TypeScript) ──► bridge-native.exe ──► virtual camera (Media Foundation / DirectShow)
Android app (Kotlin) ──USB/ADB─────┘                         └──► bridge-native.exe ──► mwbmic.sys (kernel audio driver)
```

| Component | Path | Language | Role |
|-----------|------|----------|------|
| iOS app (*Mobile Webcam*) | `ios/` | Swift 6 | Captures camera + mic, encodes H.264, serves the wire protocol on `127.0.0.1:27100` |
| Android app (*Mobile Webcam*) | `android/` | Kotlin | Camera2 → MediaCodec H.264, `AudioRecord` PCM, foreground service, serves the wire protocol on `127.0.0.1:27100` |
| Host | `host/` | TypeScript (Node 26) | Orchestrator: usbmux and ADB transports, sessions, decoding, demand, drift compensation, CLI |
| `bridge-native.exe` | `native/bridge-native/` | C++20 | Installer, video hub (frame pipes), microphone feeder, diagnostics |
| `vcam-mf.dll` | `native/vcam-mf/` | C++20 | Media Foundation virtual camera source (Windows 11) |
| `vcam-dshow.dll` | `native/vcam-dshow/` | C++20 | DirectShow capture filter (Windows 10) |
| `mwbmic.sys` | `native/virtual-mic/` | C++ (kernel) | PortCls WaveRT capture-only virtual microphone |
| Protocol specs | `protocol/` | — | Wire protocol, frame pipe, mic IOCTL, CLI contract, golden test vectors |

Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design and
[docs/SETUP.md](docs/SETUP.md) for the full setup.

## Quick start (development)

Prerequisites: Windows 11, Node 26, ffmpeg on `PATH`, Visual Studio Build Tools 2022 with the
Windows 11 SDK, WDK and Spectre libraries, and the phone side for your phone:

- **iPhone:** Apple Mobile Device Support (iTunes) and the iOS app installed on the iPhone (built by
  CI, sideloaded with Sideloadly — see `ios/README.md`).
- **Android (10+):** Android platform-tools (`winget install Google.PlatformTools`), USB debugging
  enabled on the phone, and the Android app (built by CI, installed with `adb install` — see
  `android/README.md`).

```powershell
# 1. Native components (camera DLLs, driver, bridge-native.exe)
pwsh scripts/build-native.ps1 -Configuration Release

# 2. Host
cd host
npm ci
npm run check                     # typecheck, lint, unit + integration tests

# 3. Install the virtual devices (one UAC prompt)
node src/main.ts install

# 4. Verify, then run
node src/main.ts doctor
node src/main.ts devices          # plug in the phone, open the Mobile Webcam app
node src/main.ts start
```

Open any camera app and pick **Mobile Webcam Windows Virtual Camera** (Windows 11) or
**Mobile Webcam** (Windows 10); pick **Mobile Webcam Microphone** as the input device. The phone
only streams while an app actually uses the camera or microphone.

## CLI

The CLI is `mobile-webcam-bridge`; from a checkout run it as `node src/main.ts <command>` in `host/`.

| Command | Purpose |
|---------|---------|
| `start` | Run the bridge (default) |
| `devices` | List phones (iPhone and Android), their trust or USB-debugging state, and whether the app answers |
| `probe --record <dir> --seconds N` | Record raw streams (`video.h264`, `audio.wav`, `packets.jsonl`) and print statistics |
| `doctor` | Check every prerequisite end to end |
| `status` | Show installed virtual devices |
| `install` / `uninstall` | Add or remove the virtual camera and microphone (administrator) |

Global options: `--config <file>`, `--log-level <level>`, `--device <id>` (iPhone UDID or Android
serial), `--tcp <host>` (development without USB). Configuration: copy
`host/bridge.config.example.json` to `bridge.config.json` (working directory) or
`%APPDATA%\mobile-webcam-bridge\config.json`. Sections: `device`, `usbmux`, `adb`, `video`, `audio`,
`camera`, `ffmpeg`, `native`, `logging` — see [docs/SETUP.md](docs/SETUP.md) §7.

## Status

| Milestone | State |
|-----------|-------|
| Protocol specs + golden vectors | done |
| Host (usbmux, session, pipelines, CLI, tests) | done, tested against a fake device and real ffmpeg |
| iOS app + CI | written; first compile happens in GitHub Actions |
| Android app + ADB transport | written; first build in GitHub Actions |
| `bridge-native`, camera DLLs, driver | written; first build needs the Windows SDK/WDK |
| Real-device validation (M3–M7) | pending |

## License

Original code: see the repository owner. Third-party code keeps its own license — see
[docs/THIRD_PARTY_NOTICES.md](docs/THIRD_PARTY_NOTICES.md).
