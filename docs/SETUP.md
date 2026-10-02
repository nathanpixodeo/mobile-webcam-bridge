# Setup

This guide takes a fresh Windows 11 PC to a working iPhone or Android webcam and microphone.

## 1. Windows prerequisites

1. **Node.js 26** (runs the TypeScript host directly) and **git**.
2. **ffmpeg** on `PATH` (e.g. `winget install Gyan.FFmpeg`). The host spawns it as the H.264
   decoder.
3. **Apple Mobile Device Support** (iPhone only) — installed with iTunes (the web installer from
   apple.com, not the Microsoft Store version). It provides the USB driver and usbmuxd on
   `127.0.0.1:27015`.
4. **Android platform-tools** (Android only) — `winget install Google.PlatformTools`. It provides
   `adb.exe` and the ADB server on `127.0.0.1:5037`. The host starts the server itself
   (`adb start-server`) when it finds `adb`: config `adb.path`, `PATH`, or
   `%LOCALAPPDATA%\Android\Sdk\platform-tools`.
5. **Visual Studio Build Tools 2022 (17.14+)** with the Windows 11 SDK, the WDK and the Spectre
   libraries:

   ```powershell
   & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" update `
       --installPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" --passive --norestart
   & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" modify `
       --installPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" `
       --add Microsoft.VisualStudio.Component.Windows11SDK.26100 `
       --add Microsoft.VisualStudio.Component.VC.Runtimes.x86.x64.Spectre `
       --add Microsoft.Windows.DriverKit --passive --norestart
   ```

## 2. Build the native components

```powershell
git submodule update --init
pwsh scripts/build-native.ps1 -Configuration Release
```

The script builds x64 and Win32, runs the native unit tests and assembles
`native/out/Release/stage/` (see `protocol/BRIDGE_NATIVE.md` §4). Without the WDK it skips the
driver.

## 3. The virtual microphone driver (test signing)

Kernel drivers must be signed. Until the driver is attestation-signed (EV certificate + Partner
Center), develop it in a **Hyper-V VM**; keep Secure Boot on the host. Full steps:
`native/virtual-mic/README.md`. In short:

1. Create a Generation 2 VM, disable Secure Boot for it, and run `bcdedit /set testsigning on`
   inside the VM.
2. `scripts/make-test-cert.ps1` on the host, import the `.cer` into the VM's Root and
   TrustedPublisher stores, sign with `scripts/sign-driver.ps1`.
3. Hyper-V cannot pass a phone through. On the host, forward the phone service to the VM. iPhone:
   `scripts/vm-portproxy.ps1 -HostIp <vSwitch IP> -VmSubnet <VM subnet>` and set
   `"usbmux": { "address": "<vSwitch IP>:27015" }` in the VM's `bridge.config.json`. Android: the
   same script with `-Port 5037` and `"adb": { "address": "<vSwitch IP>:5037" }`.
4. Enable Driver Verifier for `mwbmic.sys` during development.

The camera needs no signing beyond normal Authenticode for distribution; it works on the host
directly.

## 4. Install the virtual devices

```powershell
cd host
npm ci
node src/main.ts install            # one UAC prompt; camera backend chosen by Windows build
node src/main.ts status
node src/main.ts doctor
```

Options: `--camera mf|dshow|none`, `--no-mic`, `--width 1920 --height 1080 --fps 30` (the default
mode, offered first), `--max-width 3840 --max-height 2160 --max-fps 60` (the cap).
`uninstall` removes everything (`--camera` / `--mic` to remove one part).

The camera offers every catalog mode within the cap — 640×360, 640×480, 960×540, 1280×720,
1920×1080 and 2560×1440 at 15/30/60 fps, 3840×2160 at 15/30 fps — and each application picks one
(OBS: *Resolution/FPS Type → Custom*; Teams and Zoom choose on their own). The phone streams the
largest mode in use. On a slower PC, lower the cap, e.g. `--max-width 1920 --max-height 1080`.

`install` places the files in `%ProgramFiles%\MobileWebcamBridge\<version>` and registers them
under `HKLM\SOFTWARE\MobileWebcamBridge`.

## 5. The phone app

### iPhone

There is no Mac in this workflow: GitHub Actions builds an unsigned IPA and Sideloadly installs
it from Windows. See `ios/README.md`:

1. Push the repository to GitHub; the `ios` workflow builds `mobile-webcam-ipa`.
2. `gh run download -n mobile-webcam-ipa`.
3. Install iTunes and iCloud (web versions) and Sideloadly; sign in with an Apple ID (a secondary
   one is advisable) and install the IPA.
4. On the iPhone: enable Developer Mode, trust the developer profile, open **Mobile Webcam** and
   allow camera and microphone access.
5. Free Apple IDs: the app expires after 7 days; re-sign it with Sideloadly.

### Android

Android 10 or later. Some Android 14+ phones also have a built-in "USB webcam" mode (video only,
few devices); Mobile Webcam works on any Android 10+ phone and adds the microphone and remote
control. See `android/README.md`:

1. On the phone, enable **Developer options**, then **USB debugging**.
2. Install the platform-tools on Windows (`winget install Google.PlatformTools`), connect the phone
   by USB, unlock it and accept **Allow USB debugging?**. In `adb devices -l` the phone moves from
   `unauthorized` to `device`.
3. Push the repository to GitHub; GitHub Actions builds `app-debug.apk` (artifact
   `mobile-webcam-apk`, Linux runner).
4. `gh run download -n mobile-webcam-apk`, then `adb install -r app-debug.apk`.
5. Open **Mobile Webcam** and allow camera and microphone access.

## 6. Run

```powershell
node src/main.ts devices     # id (UDID or serial), state, app reachable
node src/main.ts start
```

Pick **Mobile Webcam Windows Virtual Camera** (Windows 11) or **Mobile Webcam** (Windows 10) and
**Mobile Webcam Microphone** in your apps. Logs: `%LOCALAPPDATA%\mobile-webcam-bridge\logs`. With
several phones attached the host uses the first USB phone; pin one with `--device <UDID or serial>`
or `device.id` in the config.

To check the raw phone streams without the virtual devices:

```powershell
node src/main.ts probe --seconds 10 --record recordings\first
ffplay -f h264 recordings\first\video.h264
```

## 7. Configuration

Copy `host/bridge.config.example.json` to `bridge.config.json` (working directory) or
`%APPDATA%\mobile-webcam-bridge\config.json`; `--config <file>` overrides both. Sections: `device`,
`usbmux`, `adb`, `video`, `audio`, `camera`, `ffmpeg`, `native`, `logging`. The keys for the phone
link:

| Key | Default | Purpose |
|-----|---------|---------|
| `device.port` | `27100` | Port the app listens on, on the phone |
| `device.id` | unset | Pin one phone (iPhone UDID or Android serial) |
| `usbmux.enabled` | `true` | iPhone support (Apple Mobile Device Service) |
| `usbmux.address` | `127.0.0.1:27015` | usbmuxd; point it at a forwarded port from a VM |
| `adb.enabled` | `true` | Android support |
| `adb.address` | `127.0.0.1:5037` | ADB server; point it at a forwarded port from a VM |
| `adb.path` | unset | `adb.exe` used to start the server (else `PATH`, then `%LOCALAPPDATA%\Android\Sdk\platform-tools`) |
| `adb.startServer` | `true` | Run `adb start-server` when the server is not running |
| `adb.includeNetworkDevices` | `false` | Also use phones connected by wireless debugging |

The video keys:

| Key | Default | Purpose |
|-----|---------|---------|
| `video.bitsPerPixel` | `0.1` | Bitrate = width × height × fps × this (1.5–40 Mbps; ~6 Mbps at 1080p30) |
| `video.bitrateKbps` | unset | Fixed bitrate for every mode instead |
| `video.hwaccel` | `auto` | ffmpeg decoder: `auto` uses `d3d11va` above 1080p, `none`, `d3d11va`, `dxva2` |
| `video.modeDowngradeGraceMs` | `3000` | How long a smaller mode must be all that is in use before the phone switches down |
| `camera.width`, `camera.height`, `camera.fps` | `1920`, `1080`, `30` | Default mode for `install` |
| `camera.maxWidth`, `camera.maxHeight`, `camera.maxFps` | `3840`, `2160`, `60` | Cap for `install` |
