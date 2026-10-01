import BridgeKit

/// Device facts reported in `Status`, gathered on the main actor by `SystemStatusMonitor`.
struct SystemSnapshot: Equatable, Sendable {
    var appState: AppState = .active
    var battery: BatteryStatus?
    var lowPower = false
    var permissions = PermissionsStatus(camera: .notDetermined, microphone: .notDetermined)
    var thermal: ThermalLevel = .nominal
}

enum ConnectionStatus: Equatable, Sendable {
    case starting
    case listening(port: UInt16)
    case listenerFailed(String)
    case handshaking
    case connected
}

/// What the UI shows; published by the session at most twice a second.
struct SessionSnapshot: Equatable, Sendable {
    var connection: ConnectionStatus = .starting
    var video: VideoStreamStatus = .off
    var audio: AudioStreamStatus = .off
    var roundTripMs: Int?
    var videoFps = 0
    var videoKbps = 0
    var droppedVideoFrames = 0
}

/// Crash and hang diagnostics collected by MetricKit, forwarded once per connection.
protocol DiagnosticsSource: AnyObject, Sendable {
    func takePendingReports(limit: Int) -> [String]
}
