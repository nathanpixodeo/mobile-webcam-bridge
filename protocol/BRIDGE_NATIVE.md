# `bridge-native.exe` command-line contract — version 1

`bridge-native.exe` is the only native executable Node talks to. It is an x64 console program
with an `asInvoker` manifest. Node parses its output with zod schemas
(`host/src/infrastructure/native/BridgeNativeSchemas.ts`), so this document is a contract.

## 1. Conventions

- One-shot commands print exactly **one JSON object** on stdout and exit.
- Long-running commands print **JSON lines** (one object per line) on stdout, read commands as
  JSON lines on stdin (when they take commands), and **exit when stdin reaches EOF**. This makes
  orphaned helpers impossible when Node dies.
- Human-readable diagnostics go to stderr (Node forwards them to its logger).
- Every JSON object has `"ok": true|false` (one-shot) or `"event": "<name>"` (streams).
- Failure shape: `{"ok":false,"error":{"code":"<CODE>","message":"<text>"}}`.

| Exit code | Meaning                                                    |
|----------:|------------------------------------------------------------|
| 0         | success                                                    |
| 1         | generic failure                                            |
| 2         | usage error (bad arguments)                                |
| 3         | required component not installed / device not present      |
| 4         | elevation cancelled by the user (UAC "No")                 |
| 5         | operating system not supported for the requested operation |

Error codes: `USAGE`, `NOT_INSTALLED`, `DEVICE_NOT_PRESENT`, `DEVICE_BUSY`, `ELEVATION_CANCELLED`,
`NOT_SUPPORTED_OS`, `ACCESS_DENIED`, `PIPE_IN_USE`, `IO_ERROR`, `INTERNAL`.

## 2. One-shot commands

### `bridge-native version`

```json
{"ok":true,"version":"0.1.0","abi":1}
```

### `bridge-native status`

```json
{"ok":true,
 "installDir":"C:\\Program Files\\MobileWebcamBridge\\0.1.0",
 "os":{"build":26200,"isWin11":true},
 "camera":{"installed":true,"backend":"mf","friendlyName":"Mobile Webcam","width":1280,"height":720,"fpsNum":30,"fpsDen":1,"pipeName":"mobile-webcam-bridge-video"},
 "mic":{"installed":true,"devicePresent":true,"problemCode":0}}
```

`camera.backend` ∈ `mf`, `dshow`, `none`. When the camera is not installed, `camera` is
`{"installed":false}`. When the driver is not installed, `mic` is `{"installed":false}`.

### `bridge-native install [options]`

Self-elevates (one UAC prompt), performs every step, prints the result.

| Option                           | Default  | Meaning                                         |
|----------------------------------|----------|-------------------------------------------------|
| `--camera auto\|mf\|dshow\|none` | `auto`   | `auto` = `mf` on build ≥ 22000, else `dshow`    |
| `--mic` / `--no-mic`             | `--mic`  | install the virtual microphone driver           |
| `--width N --height N`           | 1280×720 | camera mode (whitelist: 640×360, 1280×720, 1920×1080) |
| `--fps N`                        | 30       | 15, 24, 25, 30, 60                              |
| `--name TEXT`                    | `Mobile Webcam` | camera friendly name (MF appends "Windows Virtual Camera") |

```json
{"ok":true,"version":"0.1.0","installDir":"C:\\Program Files\\MobileWebcamBridge\\0.1.0",
 "steps":[{"name":"copy-files","ok":true},{"name":"register-mf-source","ok":true},
          {"name":"create-virtual-camera","ok":true},{"name":"install-mic-driver","ok":true}]}
```

A failed step stops the run: `"ok":false`, the failing step carries `"error":{code,message}`,
and steps already done are rolled back where possible (reported as `{"name":"rollback-…"}`).

### `bridge-native uninstall [--camera] [--mic]`

Self-elevates. Without flags it removes everything. Same output shape as `install`.

### `bridge-native doctor`

```json
{"ok":true,"checks":[{"id":"os.build","status":"pass","message":"Windows build 26200"},
                     {"id":"camera.privacy","status":"warn","message":"..."}]}
```

`status` ∈ `pass`, `warn`, `fail`. `ok` is false if any check fails. Check ids:
`os.build`, `frameserver.service`, `camera.registered`, `camera.files`, `camera.privacy`,
`camera.duplicateBackends`, `mic.driver`, `mic.problemCode`, `mic.endpoint`, `mic.privacy`,
`system.testSigning`, `system.secureBoot`, `pipe.free`.

### `bridge-native video watch [--frames N] [--timeout-ms T] [--pipe-name NAME]`

Connects to the public pipe as a consumer (diagnostics and integration tests).

```json
{"ok":true,"frames":30,"placeholderFrames":0,"width":1280,"height":720,"elapsedMs":1003}
```

### `bridge-native mic status`

One-shot: `{"ok":true,"present":true,"busy":false,"bufferedBytes":0,"streamActive":false}`.
`busy: true` (and no buffer fields) when a feeder holds the exclusive handle.

## 3. Long-running commands

### `bridge-native video hub --ingest-pipe PATH [--pipe-name NAME] [--width N --height N --fps-num N --fps-den N]`

`--ingest-pipe` takes the full path (`\\.\pipe\mobile-webcam-bridge-ingest-<token>`); `--pipe-name` takes the public pipe name without the `\\.\pipe\` prefix.

Mode defaults to the installed camera mode (registry); the explicit options exist for tests.

**stdout events**

```json
{"event":"ready","ingestPipe":"\\\\.\\pipe\\mobile-webcam-bridge-ingest-0123456789abcdef","publicPipe":"\\\\.\\pipe\\mobile-webcam-bridge-video","width":1280,"height":720,"fpsNum":30,"fpsDen":1}
{"event":"consumers","count":1}
{"event":"ingest","connected":true}
{"event":"stats","framesIn":300,"framesOut":300,"consumerDrops":0,"placeholderFrames":12,"consumers":1}
{"event":"error","code":"PIPE_IN_USE","message":"...","fatal":true}
```

- `consumers` is emitted on every change of the consumer count.
- `stats` is emitted every 5 s.
- A fatal error is followed by exit code 1.

**stdin commands**

```json
{"cmd":"loadPlaceholder","kind":"no-device","path":"C:\\Users\\me\\AppData\\Local\\mobile-webcam-bridge\\cache\\no-device-1280x720.nv12"}
{"cmd":"placeholder","kind":"no-device"}
{"cmd":"placeholder","kind":null}
```

- `loadPlaceholder`: reads the file (must be exactly `width × height × 3 / 2` bytes) and stores
  it under `kind`. Kinds: `no-device`, `app-closed`, `paused`, `background`, `stopped`.
- `placeholder` with a kind: show that placeholder until told otherwise.
- `placeholder` with `null`: show live ingest frames; if no ingest frame arrives for 300 ms, show
  the `stopped` placeholder (or a built-in neutral frame if none is loaded).

### `bridge-native mic feed`

Reads raw PCM (`s16le`, 48 kHz, mono) from stdin and pushes it into `\\.\MobileWebcamBridgeMic` in
10 ms chunks.

```json
{"event":"ready","abi":1,"sampleRate":48000,"channels":1,"capacityBytes":65536}
{"event":"status","bufferedBytes":3840,"capacityBytes":65536,"streamActive":true,"underruns":0,"overruns":0}
{"event":"error","code":"DEVICE_NOT_PRESENT","message":"...","fatal":true}
```

- `status` every 100 ms.
- Exit code 3 when the device is missing, 1 with `DEVICE_BUSY` when another feeder holds it.

## 4. Build output and staging layout

Every user-mode project builds into `native/out/<Configuration>/<Platform>/` (`Platform` is
`x64` or `Win32`); the driver package lands in `native/out/<Configuration>/x64/virtual-mic/`.
`scripts/build-native.ps1` then assembles the **stage** folder that `install` copies from (the
folder containing the running `bridge-native.exe`):

```
native/out/<Configuration>/stage/
  bridge-native.exe
  vcam-mf.dll
  vcam-dshow.dll            (x64)
  x86/vcam-dshow.dll        (Win32)
  driver/mwbmic.sys
  driver/mwbmic.inf
  driver/mwbmic.cat
```

`install` copies the stage folder to `C:\Program Files\MobileWebcamBridge\<version>\` and records that
path as `InstallDir` (plus `Version`) in `HKLM\SOFTWARE\MobileWebcamBridge`. `status.installDir` is `null` when
nothing is installed. Node runs the installed `bridge-native.exe` when present, otherwise the
stage copy.
