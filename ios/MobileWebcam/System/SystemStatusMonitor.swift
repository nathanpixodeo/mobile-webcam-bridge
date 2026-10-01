import AVFoundation
import BridgeKit
import UIKit

/// Aggregates the device facts reported in `Status` (power, permissions, app state, thermal)
/// into one `SystemSnapshot` and publishes it whenever something changes.
@MainActor
final class SystemStatusMonitor {
    private(set) var snapshot = SystemSnapshot()
    private let onChange: @MainActor (SystemSnapshot) -> Void
    private var tokens: [any NSObjectProtocol] = []

    init(onChange: @escaping @MainActor (SystemSnapshot) -> Void) {
        self.onChange = onChange
    }

    func start() {
        guard tokens.isEmpty else { return }
        UIDevice.current.isBatteryMonitoringEnabled = true
        let names = [
            UIDevice.batteryLevelDidChangeNotification,
            UIDevice.batteryStateDidChangeNotification,
            Notification.Name.NSProcessInfoPowerStateDidChange,
        ]
        tokens = names.map { name in
            NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                guard let self else { return }
                Task { @MainActor in self.refreshPower() }
            }
        }
        refreshPower()
        refreshPermissions()
    }

    func update(appState: AppState) {
        mutate { $0.appState = appState }
        // Permissions may have been changed in Settings while the app was away.
        if appState == .active { refreshPermissions() }
    }

    func update(thermal: ThermalLevel) {
        mutate { $0.thermal = thermal }
    }

    func refreshPermissions() {
        let permissions = PermissionsStatus(
            camera: Self.permission(AVCaptureDevice.authorizationStatus(for: .video)),
            microphone: Self.permission(AVCaptureDevice.authorizationStatus(for: .audio))
        )
        mutate { $0.permissions = permissions }
    }

    private func refreshPower() {
        let device = UIDevice.current
        let battery: BatteryStatus? = device.batteryLevel < 0 ? nil : BatteryStatus(
            levelPercent: Int((device.batteryLevel * 100).rounded()),
            charging: device.batteryState == .charging || device.batteryState == .full
        )
        let lowPower = ProcessInfo.processInfo.isLowPowerModeEnabled
        mutate {
            $0.battery = battery
            $0.lowPower = lowPower
        }
    }

    private func mutate(_ change: (inout SystemSnapshot) -> Void) {
        var next = snapshot
        change(&next)
        guard next != snapshot else { return }
        snapshot = next
        onChange(next)
    }

    private static func permission(_ status: AVAuthorizationStatus) -> PermissionState {
        switch status {
        case .authorized: .authorized
        case .denied: .denied
        case .restricted: .restricted
        case .notDetermined: .notDetermined
        @unknown default: .denied
        }
    }
}
