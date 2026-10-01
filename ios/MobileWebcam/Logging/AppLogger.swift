import BridgeKit
import Foundation
import os

/// Thread-safe logger. Every entry goes to the unified log and into two rings: one drained by
/// the session and forwarded to the host (the only way to read logs without a Mac), one kept
/// for the on-device diagnostics screen.
final class AppLogger: Sendable {
    private struct Rings: Sendable {
        var forwarding = LogRing(capacity: 1_000)
        var tail = LogRing(capacity: 300)
    }

    private let unifiedLog: Logger
    private let clock: any MonotonicClock
    private let rings = OSAllocatedUnfairLock(initialState: Rings())

    init(subsystem: String, clock: any MonotonicClock) {
        unifiedLog = Logger(subsystem: subsystem, category: "bridge")
        self.clock = clock
    }

    func log(_ level: LogLevel, _ category: String, _ message: String) {
        let entry = LogEntry(timestampUs: clock.nowMicroseconds(), level: level, category: category, message: message)
        rings.withLock { rings in
            rings.forwarding.append(entry)
            rings.tail.append(entry)
        }
        switch level {
        case .debug: unifiedLog.debug("[\(category, privacy: .public)] \(message, privacy: .public)")
        case .info: unifiedLog.info("[\(category, privacy: .public)] \(message, privacy: .public)")
        case .warn: unifiedLog.warning("[\(category, privacy: .public)] \(message, privacy: .public)")
        case .error: unifiedLog.error("[\(category, privacy: .public)] \(message, privacy: .public)")
        }
    }

    func debug(_ category: String, _ message: String) { log(.debug, category, message) }
    func info(_ category: String, _ message: String) { log(.info, category, message) }
    func warn(_ category: String, _ message: String) { log(.warn, category, message) }
    func error(_ category: String, _ message: String) { log(.error, category, message) }

    /// Oldest entries not yet forwarded, plus how many were lost to ring overflow.
    func takeForwardingBatch(maxCount: Int) -> (entries: [LogEntry], dropped: Int) {
        rings.withLock { rings in
            (rings.forwarding.drain(maxCount: maxCount), rings.forwarding.takeDroppedCount())
        }
    }

    func recentEntries(limit: Int) -> [LogEntry] {
        rings.withLock { $0.tail.newest(limit) }
    }
}
