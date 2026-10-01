# bridge-native

Native helper of the Mobile Webcam Bridge host: installs the camera and microphone components and runs
the two data paths that must not live in Node (frame pipes, microphone feed). The command-line
contract is `protocol/BRIDGE_NATIVE.md`; frame and microphone formats are in
`protocol/FRAME_PIPE.md` and `protocol/MIC_FEED.md`.

## Layout

| Folder              | Responsibility                                                                 |
|---------------------|--------------------------------------------------------------------------------|
| `src/core`          | `Command`/`Application` dispatch, strict `ArgParser`, JSON writer/parser, errors, output channels |
| `src/platform`      | Win32 wrappers: paths, registry views, SDDL, overlapped named pipes, stdin, process/elevation state |
| `src/elevation`     | `ElevationBroker`: one UAC prompt, result returned over a random private pipe  |
| `src/install`       | `InstallService` builds an `InstallPlan` of `InstallStep`s; `StepRunner` runs it transactionally (install) or best effort (uninstall) |
| `src/doctor`        | One `DoctorCheck` per check id, read-only `SystemProbes`                       |
| `src/video`         | `VideoHub` = `IngestServer` + `ConsumerServer` + `OutputPolicy` + `PlaceholderStore`; `FrameWatcher` for `video watch` |
| `src/mic`           | `MicDevice` (control device IOCTLs), `MicFeeder`, `PcmChunker`                 |
| `src/commands`      | Thin `Command` classes wiring options to the services                          |

Pure logic (`Json`, `ArgParser`, `FrameAssembler`, `ConsumerSlot`, `OutputPolicy`, `HubCommand`,
`PcmChunker`, `LineSplitter`, `InstallOptions`) is covered by `native/tests`.

## Threading of `video hub`

- Ingest thread: accepts one writer at a time, reads straight into pooled frame buffers.
- Accept thread: creates public pipe instances, starts one `ConsumerConnection` thread per client,
  reaps finished ones and reports the consumer count.
- Consumer threads: write header + payload; a pending one-byte read detects disconnects.
- Main thread: 50 ms tick for the 300 ms idle fallback and 5 s stats; stdin thread for commands.

Lock order is hub → consumer server → connection; the accept thread fetches the latest frame
before taking the server lock.

## Build

`scripts/build-native.ps1` (needs Visual Studio 2022 Build Tools with the Windows 11 SDK).
