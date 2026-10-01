/// Liveness rules of SPEC §2.4: ping every second once handshaken, give up after 4 s without
/// any inbound packet.
public struct HeartbeatWatchdog: Sendable {
    public enum Action: Equatable, Sendable {
        case sendPing
        case timedOut
    }

    public let pingIntervalUs: Int64
    public let timeoutUs: Int64
    private var lastInboundUs: Int64?
    private var lastPingUs: Int64?

    public init(pingIntervalUs: Int64 = 1_000_000, timeoutUs: Int64 = 4_000_000) {
        self.pingIntervalUs = pingIntervalUs
        self.timeoutUs = timeoutUs
    }

    public var isRunning: Bool { lastInboundUs != nil }

    public mutating func start(nowUs: Int64) {
        lastInboundUs = nowUs
        lastPingUs = nil
    }

    public mutating func stop() {
        lastInboundUs = nil
        lastPingUs = nil
    }

    public mutating func noteInbound(nowUs: Int64) {
        guard isRunning else { return }
        lastInboundUs = nowUs
    }

    /// Call periodically (every ~100 ms).
    public mutating func tick(nowUs: Int64) -> Action? {
        guard let lastInbound = lastInboundUs else { return nil }
        if nowUs - lastInbound >= timeoutUs {
            stop()
            return .timedOut
        }
        if let lastPing = lastPingUs, nowUs - lastPing < pingIntervalUs { return nil }
        lastPingUs = nowUs
        return .sendPing
    }
}
