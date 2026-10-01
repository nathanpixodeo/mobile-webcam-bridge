// Product-wide identifiers shared by every user-mode native component. Changing any value here
// breaks upgrades of existing installations, so treat them as frozen.
#pragma once

namespace mwb::ids {

inline constexpr wchar_t kProductName[] = L"MobileWebcamBridge";
inline constexpr wchar_t kProductVersion[] = L"0.1.0";
inline constexpr int kAbiVersion = 1;

// COM class of the Media Foundation virtual camera source (vcam-mf.dll).
inline constexpr wchar_t kMfSourceClsid[] = L"{39F0E089-E94D-41FA-BD58-7E0532B0B652}";
// COM class of the DirectShow capture filter (vcam-dshow.dll).
inline constexpr wchar_t kDShowFilterClsid[] = L"{4FE59245-46F3-4EBD-9134-54C688D28A7D}";

// Registry (HKLM, always accessed with KEY_WOW64_64KEY so the x86 DirectShow DLL sees the same
// keys). Written by `bridge-native install`, removed by `uninstall`.
//   SOFTWARE\MobileWebcamBridge          InstallDir (REG_SZ), Version (REG_SZ)
//   SOFTWARE\MobileWebcamBridge\Camera   Backend (REG_SZ "mf"|"dshow"), FriendlyName (REG_SZ),
//                                  Width, Height, FpsNum, FpsDen (REG_DWORD), PipeName (REG_SZ)
inline constexpr wchar_t kProductRegistryKey[] = L"SOFTWARE\\MobileWebcamBridge";
inline constexpr wchar_t kCameraRegistryKey[] = L"SOFTWARE\\MobileWebcamBridge\\Camera";

inline constexpr wchar_t kDefaultFriendlyName[] = L"Mobile Webcam";
inline constexpr wchar_t kDShowFriendlyName[] = L"Mobile Webcam";

// Installation root; each version gets its own subfolder because Frame Server and client apps
// keep the camera DLLs loaded.
inline constexpr wchar_t kInstallSubdirectory[] = L"MobileWebcamBridge";

}  // namespace mwb::ids
