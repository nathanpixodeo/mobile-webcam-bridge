# virtual-mic — "Mobile Webcam Microphone" kernel driver

`mwbmic.sys` is a PortCls WaveRT audio driver for a root-enumerated virtual device
(`ROOT\MobileWebcamBridgeMic`). It exposes one capture endpoint, **Mobile Webcam Microphone**
(48 kHz, 16-bit, mono), which any Windows app can select as a microphone.

Audio gets in through a separate **control device**, `\\.\MobileWebcamBridgeMic`. `bridge-native mic feed`
opens it and pushes PCM with `IOCTL_MWBMIC_WRITE` (ABI: [`protocol/MIC_FEED.md`](../../protocol/MIC_FEED.md),
[`MwbMicIoctl.h`](../common/include/mwb/MwbMicIoctl.h)).

```
bridge-native mic feed ──IOCTL_MWBMIC_WRITE──► control device ──► MicFeedBuffer (64 KiB ring)
                                                                        │ read on every position update
Windows audio engine ◄── WaveRT capture stream (1 ms timer "DMA") ◄─────┘
```

The driver is deliberately dumb:

- Writes are discarded while no app captures (`STREAM_ACTIVE` clear).
- The oldest audio is dropped on overrun.
- The capture stream reads silence on underrun.
- The ring is flushed when capture starts and when the feeder's handle closes.

Clock drift between the phone and the driver timer is compensated in user mode: Node resamples to
keep `BufferedBytes` near 40 ms.

## Layout

| Path | Role |
|------|------|
| `src/adapter.cpp` | `DriverEntry`, `AddDevice`, `StartDevice`; routes every IRP to the control device or PortCls |
| `src/common.cpp` | `CAdapterCommon`: installs the endpoint, owns the `MicFeedBuffer` |
| `src/ControlDevice.*` | `\Device\MobileWebcamBridgeMic`: exclusive open, IOCTLs, rundown-protected access to the feed |
| `src/MicFeedBuffer.*` | spinlock-protected `mwb::PcmRing` + counters |
| `src/minwavert*.cpp` | WaveRT miniport and capture stream (`WriteBytes()` copies from the feed) |
| `src/mictopo.cpp`, `*table.h`, `minipairs.h` | topology miniport, filter descriptors, endpoint table |
| `mwbmic.inx` | INF template; StampInf produces `mwbmic.inf` |

The derived files and their license are listed in [`NOTICE.md`](NOTICE.md) (MS-PL).

## Build

Prerequisites (one-time): VS 2022 Build Tools with the **Windows 11 SDK 10.0.26100**, the
**Windows Driver Kit** component and the **Spectre-mitigated libraries** (see `docs/SETUP.md`).

```powershell
# whole native solution (stages everything under native\out\<Config>\stage)
scripts\build-native.ps1 -Configuration Release

# or only the driver
msbuild native\virtual-mic\virtual-mic.vcxproj /p:Configuration=Debug /p:Platform=x64
```

Output: `native\out\<Config>\x64\virtual-mic\` with `mwbmic.sys`, `mwbmic.inf` and
`mwbmic.cat`. The build runs Inf2Cat and InfVerif. It never signs anything unless asked.

## Test signing

Kernel drivers must be signed. During development, use a self-signed test certificate in a VM:

```powershell
scripts\make-test-cert.ps1            # creates "MobileWebcamBridge Test Driver Signing" in CurrentUser\My
                                      # and exports native\out\MobileWebcamBridgeTest.cer
$env:MWB_TEST_CERT_THUMBPRINT = '<thumbprint printed above>'
msbuild native\virtual-mic\virtual-mic.vcxproj /p:Configuration=Debug /p:Platform=x64   # signs .sys + .cat

# or sign an existing package
scripts\sign-driver.ps1 -PackageDir native\out\Debug\x64\virtual-mic -Thumbprint <thumbprint>
```

## Development VM (Hyper-V)

Keep Secure Boot on the development PC. Test the driver in a VM, where crashes are cheap.

1. Create a **Generation 2** Windows 11 VM with nested virtualization, so HVCI can run inside it:
   ```powershell
   Set-VMFirmware  -VMName mwb-driver -EnableSecureBoot Off
   Set-VMProcessor -VMName mwb-driver -ExposeVirtualizationExtensions $true
   ```
2. In the VM (elevated):
   ```powershell
   bcdedit /set testsigning on
   Import-Certificate -FilePath .\MobileWebcamBridgeTest.cer -CertStoreLocation Cert:\LocalMachine\Root
   Import-Certificate -FilePath .\MobileWebcamBridgeTest.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
   ```
   Turn on **Memory integrity** (Windows Security → Device security → Core isolation) so HVCI
   checks the driver too. Reboot, then take a checkpoint.
3. Kernel debugging over the network (KDNET):
   ```powershell
   # VM
   bcdedit /debug on
   bcdedit /dbgsettings net hostip:<host IP> port:50005 key:1.2.3.4
   # host (winget install Microsoft.WinDbg)
   windbgx -k net:port=50005,key=1.2.3.4
   ```
4. Driver Verifier (standard checks include IRQL checking, pool tracking and special pool):
   ```powershell
   verifier /standard /driver mwbmic.sys
   # reboot; verify with: verifier /query
   ```

### Using the phone inside the VM

Hyper-V has no USB passthrough. Keep the phone on the host and forward the host's device
service to the VM over the internal switch only — usbmuxd (Apple Mobile Device Service,
`127.0.0.1:27015`) for an iPhone, the ADB server (`127.0.0.1:5037`) for an Android phone. Run this
on the host, elevated (`scripts/vm-portproxy.ps1 -Port 27015|5037` automates it):

```powershell
netsh interface portproxy add v4tov4 listenaddress=<host vEthernet IP> listenport=27015 connectaddress=127.0.0.1 connectport=27015
New-NetFirewallRule -DisplayName "usbmuxd for mwb VM" -Direction Inbound -LocalAddress <host vEthernet IP> -LocalPort 27015 -Protocol TCP -Action Allow
```

In the VM, point the host app at it with `usbmux.address = "<host vEthernet IP>:27015"` (iPhone)
or `adb.address = "<host vEthernet IP>:5037"` (Android). Remove the rule when you are done:
anything that reaches the port can talk to the phone.

## Install and test

```powershell
bridge-native install --mic --camera none     # one UAC prompt; stages the INF, creates ROOT\MobileWebcamBridgeMic
bridge-native mic status                      # {"ok":true,"present":true,...}
```

Check that **Device Manager → Sound, video and game controllers** lists "Mobile Webcam Bridge Audio", and
that **Settings → System → Sound → Input** lists "Mobile Webcam Microphone".

Feed a test tone and record it back:

```powershell
# terminal 1: start a capture so the stream is active (any recorder works, e.g. Voice Recorder)
ffmpeg -f dshow -i audio="Mobile Webcam Microphone (Mobile Webcam Bridge Audio)" -t 10 tone.wav
# terminal 2: feed 440 Hz
ffmpeg -f lavfi -i sine=f=440:r=48000 -ac 1 -f s16le - | bridge-native mic feed
```

`tone.wav` should contain a clean 440 Hz tone. The `status` lines from `mic feed` show
`streamActive: true`, a steady `bufferedBytes`, and no growing `underruns`/`overruns` once Node's
drift compensation runs.

Uninstall: `bridge-native uninstall --mic`.

## Release signing

Test-signed drivers only load with test signing on, and that requires Secure Boot off. To ship:

1. Buy an **EV code signing certificate** (lead time: days to weeks) and register a
   **Partner Center hardware** account with it.
2. Build Release and put `mwbmic.sys`, `mwbmic.inf` and `mwbmic.cat` in a CAB.
   Sign the CAB with the EV certificate (`signtool sign /fd sha256 /tr <timestamp URL> /td sha256`).
3. Submit it for **attestation signing** (Windows 10/11 client). Microsoft returns the package
   signed by the Windows Hardware Compatibility Publisher. That signature is valid on client
   Windows with Secure Boot and HVCI, but not on Windows Server.

## Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| Device Manager code 52 | Signature not trusted: test signing off, or the cert is missing from Root/TrustedPublisher |
| Code 10 / start failure | `StartDevice` failed: debug with KDNET; `DPF` output shows the failing step |
| `mic feed` exits with `DEVICE_BUSY` | Another feeder holds the exclusive handle |
| `mic feed` exits with `DEVICE_NOT_PRESENT` | Driver not installed or device disabled |
| Silence in apps | No feeder running, or `streamActive` false (no app is capturing yet) |
