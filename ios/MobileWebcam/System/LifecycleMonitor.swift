import BridgeKit
import UIKit

/// Reports foreground/background transitions. The camera stops in the background (an iOS rule
/// for apps without the multitasking camera entitlement); the host shows a placeholder.
@MainActor
final class LifecycleMonitor {
    private let onChange: @MainActor (AppState) -> Void
    private var tokens: [any NSObjectProtocol] = []

    init(onChange: @escaping @MainActor (AppState) -> Void) {
        self.onChange = onChange
    }

    func start() {
        guard tokens.isEmpty else { return }
        let names = [
            UIApplication.didBecomeActiveNotification,
            UIApplication.willResignActiveNotification,
            UIApplication.didEnterBackgroundNotification,
            UIApplication.willEnterForegroundNotification,
        ]
        tokens = names.map { name in
            NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                guard let self else { return }
                Task { @MainActor in self.publish() }
            }
        }
        publish()
    }

    private func publish() {
        let state: AppState = switch UIApplication.shared.applicationState {
        case .active: .active
        case .inactive: .inactive
        case .background: .background
        @unknown default: .inactive
        }
        onChange(state)
    }
}

/// Keeps the screen from locking while a host is connected, so capture keeps running.
@MainActor
final class IdleTimerController {
    func setKeepAwake(_ keepAwake: Bool) {
        guard UIApplication.shared.isIdleTimerDisabled != keepAwake else { return }
        UIApplication.shared.isIdleTimerDisabled = keepAwake
    }
}
