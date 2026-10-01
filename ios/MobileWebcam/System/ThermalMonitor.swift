import BridgeKit
import Foundation

/// Reports the process thermal state. The camera's own system-pressure level is observed by
/// `CameraVideoSource`; the video pipeline throttles on the worse of the two.
@MainActor
final class ThermalMonitor {
    private let onChange: @MainActor (ThermalLevel) -> Void
    private var token: (any NSObjectProtocol)?

    init(onChange: @escaping @MainActor (ThermalLevel) -> Void) {
        self.onChange = onChange
    }

    func start() {
        guard token == nil else { return }
        token = NotificationCenter.default.addObserver(
            forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor in self.publish() }
        }
        publish()
    }

    private func publish() {
        onChange(ThermalLevel(ProcessInfo.processInfo.thermalState))
    }
}

extension ThermalLevel {
    init(_ state: ProcessInfo.ThermalState) {
        switch state {
        case .nominal: self = .nominal
        case .fair: self = .fair
        case .serious: self = .serious
        case .critical: self = .critical
        @unknown default: self = .critical
        }
    }
}
