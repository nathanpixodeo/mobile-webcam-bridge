# Troubleshooting

Start with `node src/main.ts doctor`; every check prints what to fix.

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `iphone.service` warns | Apple Mobile Device Service not installed or stopped (needed only for iPhones) | Install iTunes (web version); `Restart-Service "Apple Mobile Device Service"` |
| `android.adb` warns, or `ADB_UNAVAILABLE` in the log | The ADB server is not running (the host starts it only when it finds `adb`, and `doctor` never does); needed only for Android phones | Install Android platform-tools (`winget install Google.PlatformTools`) and run `adb start-server`, or set `adb.path` |
| `phones.service` fails | Neither Apple Mobile Device Service (iPhone) nor the ADB server (Android) is reachable | Set up the service for the phone you use (the two rows above); a warning for the other one is fine |
| `devices` says "No phone found", or the camera shows "Connect your phone" | Cable, locked phone, untrusted computer (iPhone), USB debugging off (Android) | Unlock the phone and reconnect; iPhone: tap **Trust**; Android: see the ADB rows below |
| `devices` says "not trusted" (`iphone.trust` fails) | Pairing record missing (iPhone) | Reconnect and accept the Trust prompt; reset in Settings › General › Transfer or Reset › Reset Location & Privacy if it never appears |
| App column says "not running" | Mobile Webcam app closed | Open the app; the host polls every second |
| Camera shows "Open Mobile Webcam" | Session not established | Same as above; check the app's diagnostics screen |
| Camera shows "Mobile Webcam is in the background" | iOS stops the camera in the background | Keep the app in the foreground (use its standby screen to save power) |
| Camera shows "Camera paused" | The phone camera is busy or interrupted | It resumes automatically |
| Camera shows "Starting camera..." for long | No frames from the decoder | Check host logs for `ffmpeg` errors; run `probe` to verify the phone stream |
| Camera missing in apps | Not installed, or camera privacy off | `status`; Settings › Privacy › Camera › allow desktop apps |
| Camera listed twice | Both backends installed after a Windows upgrade | `uninstall --camera`, then `install` |
| Teams preview works but the other side sees nothing | Frame Server compatibility | Report with logs; this is milestone M4's gate |
| `HUB_IN_USE` | Another bridge instance is running | Close it; only one host may run |
| No microphone | Driver not installed or unsigned (code 52) | `status`; see `native/virtual-mic/README.md` for test signing |
| Microphone crackles every few minutes | Clock drift not converging | Check `audioDriftPpm` and `audioFillMs` in the "Stream health" log line; report the values |
| `ELEVATION_CANCELLED` | UAC prompt declined | Run `install` again and accept |
| Sideloaded iPhone app stops opening | Free Apple ID signature expired (7 days) | Re-sign with Sideloadly |
| Android phone is `unauthorized` (`android.device` fails) | The "Allow USB debugging?" prompt was not accepted | Unlock the phone and accept the prompt; `adb kill-server`, then reconnect; if it never appears, use **Revoke USB debugging authorizations** in Developer options and reconnect |
| Android phone is `offline` (`android.device` warns) | Stale or half-open ADB connection | Reconnect the cable; toggle USB debugging off and on; `adb kill-server`, then reconnect |
| Android phone listed, app "not running" | Mobile Webcam is not open, so nothing listens on port 27100 | Open **Mobile Webcam** on the phone; the host polls every second |
| "adb server version (NN) doesn't match this client (NN)" | Two different `adb` versions fight over the server | Use one `adb`: point `adb.path` to the same platform-tools that Android Studio uses |
| Android stream stops after a while or with the screen off | Battery optimisation killed the foreground service | Exclude Mobile Webcam from battery optimisation (menu names vary by manufacturer) |

Useful commands:

```powershell
node src/main.ts start --log-level debug
node src/main.ts probe --seconds 10 --no-audio
Get-Content "$env:LOCALAPPDATA\mobile-webcam-bridge\logs\bridge-$(Get-Date -Format yyyy-MM-dd).log" -Tail 100
```

On Android:

```powershell
adb devices -l                 # device, unauthorized or offline, plus model
adb logcat -s MobileWebcam     # the app's own log
```

On the iPhone without a Mac, `idevicesyslog` and `idevicecrashreport` from libimobiledevice read
device logs and crash reports. The host also prints the app's own logs (component `ios` or
`android`).
