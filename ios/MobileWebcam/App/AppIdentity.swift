import BridgeKit
import UIKit

/// Facts about this build and device, gathered once on the main actor and passed around as a
/// value afterwards.
struct AppIdentity: Sendable {
    static let appName = "mobile-webcam-bridge-ios"
    static let features = ["audio.pcm", "log", "status", "video.h264"]

    let app: AppDescriptor
    let device: DeviceDescriptor

    @MainActor
    static func current(bundle: Bundle = .main) -> AppIdentity {
        let info = bundle.infoDictionary ?? [:]
        let device = UIDevice.current
        return AppIdentity(
            app: AppDescriptor(
                name: appName,
                version: info["CFBundleShortVersionString"] as? String ?? "0.0.0",
                build: info["CFBundleVersion"] as? String ?? "0",
                gitSha: info["GitCommit"] as? String ?? "unknown"
            ),
            device: DeviceDescriptor(
                model: hardwareModel(),
                name: device.name,
                os: "\(device.systemName) \(device.systemVersion)"
            )
        )
    }

    var hello: HelloMessage {
        HelloMessage(role: .device, app: app, device: device, features: Self.features)
    }

    /// Machine identifier such as `iPhone16,1` (UIDevice only reports "iPhone").
    private static func hardwareModel() -> String {
        var systemInfo = utsname()
        uname(&systemInfo)
        return withUnsafeBytes(of: &systemInfo.machine) { raw in
            String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
        }
    }
}
