# Third-party notices

| Component | Used in | License | Notice |
|-----------|---------|---------|--------|
| [microsoft/wil](https://github.com/microsoft/wil) (git submodule) | `native/*` user-mode code | MIT | `native/third_party/wil/LICENSE` |
| [doctest](https://github.com/doctest/doctest) | `native/tests` | MIT | `native/third_party/doctest/LICENSE.txt` |
| DirectShow BaseClasses (from [tshino/softcam](https://github.com/tshino/softcam), originally Windows-classic-samples) | `native/vcam-dshow` | MIT | `native/third_party/baseclasses/LICENSE` |
| [microsoft/Windows-Camera VirtualCamera sample](https://github.com/microsoft/Windows-Camera/tree/master/Samples/VirtualCamera) | design reference for `native/vcam-mf` | MIT | `native/vcam-mf/NOTICE.md` |
| [smourier/VCamSample](https://github.com/smourier/VCamSample) | design reference for `native/vcam-mf` | MIT | `native/vcam-mf/NOTICE.md` |
| [Windows-driver-samples / audio / simpleaudiosample](https://github.com/microsoft/Windows-driver-samples/tree/main/audio/simpleaudiosample) | basis of `native/virtual-mic` (derived files keep their MS-PL headers) | MS-PL | `native/virtual-mic/NOTICE.md`, `native/virtual-mic/LICENSE-MS-PL.txt` |
| [zod](https://github.com/colinhacks/zod), [pino](https://github.com/pinojs/pino), [pino-pretty](https://github.com/pinojs/pino-pretty), [plist](https://github.com/TooTallNate/plist.js) | `host` runtime dependencies | MIT | `host/node_modules/*/LICENSE` |
| [AndroidX](https://developer.android.com/jetpack/androidx), [Jetpack Compose](https://developer.android.com/jetpack/compose), [Kotlin standard library](https://kotlinlang.org/), [kotlinx.coroutines](https://github.com/Kotlin/kotlinx.coroutines) | `android/` app dependencies (versions in `android/gradle/libs.versions.toml`) | Apache-2.0 | resolved by Gradle at build time, not vendored; license text at https://www.apache.org/licenses/LICENSE-2.0 |
| [JUnit 5](https://junit.org/junit5/) | `android/` unit tests only (not shipped) | EPL-2.0 | resolved by Gradle at build time |
| [FFmpeg](https://ffmpeg.org) | invoked as an external process by the host (not linked or redistributed) | LGPL/GPL depending on the build | the user's own installation |

The MS-PL is not compatible with the GPL; keep driver code out of GPL-licensed distributions.
