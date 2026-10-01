# DirectShow BaseClasses (vendored)

The DirectShow base classes (`CSource`, `CSourceStream`, `CBaseFilter`, COM plumbing, filter
registration helpers) as trimmed by [tshino/softcam](https://github.com/tshino/softcam), taken
unmodified from `src/baseclasses` at commit `1cff09074238b7229d6e2aae57601f31a239006d`
(2023-11-30). They originate from Microsoft's
[Windows-classic-samples](https://github.com/microsoft/Windows-classic-samples) and are MIT
licensed — see `LICENSE`.

Used only by `native/vcam-dshow`. The files are compiled as part of that project with relaxed
warnings (they predate `/W4 /WX /permissive-` and rely on the Windows `min`/`max` macros), and the
directory is an *external* include path so its headers do not trigger warnings in our code.

Do not edit these files; if a change is unavoidable, record it here.
