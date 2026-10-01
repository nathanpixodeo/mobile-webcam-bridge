import BridgeKit
import Observation

/// User actions the UI can trigger; implemented by the composition root.
@MainActor
protocol AppActions: AnyObject {
    func setTestPatternEnabled(_ enabled: Bool)
    func requestPermissions() async
    func recentLogs() -> [LogEntry]
}

/// Observable UI state. Session and system snapshots are pushed in; user intent goes out through
/// `AppActions`.
@MainActor
@Observable
final class AppModel {
    let identity: AppIdentity
    let port: UInt16

    private(set) var session = SessionSnapshot()
    private(set) var system = SystemSnapshot()
    private(set) var recentLogs: [LogEntry] = []
    var isStandby = false
    var useTestPattern = false

    @ObservationIgnored weak var actions: (any AppActions)?

    init(identity: AppIdentity, port: UInt16) {
        self.identity = identity
        self.port = port
    }

    var needsPermissions: Bool {
        system.permissions.camera != .authorized || system.permissions.microphone != .authorized
    }

    func apply(_ snapshot: SessionSnapshot) {
        session = snapshot
    }

    func apply(_ snapshot: SystemSnapshot) {
        system = snapshot
    }

    func testPatternChanged() {
        actions?.setTestPatternEnabled(useTestPattern)
    }

    func requestPermissions() async {
        await actions?.requestPermissions()
    }

    func refreshLogs() {
        recentLogs = actions?.recentLogs() ?? []
    }
}
