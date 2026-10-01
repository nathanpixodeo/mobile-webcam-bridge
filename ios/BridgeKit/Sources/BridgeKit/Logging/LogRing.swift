/// One log record kept for forwarding to the host and for the diagnostics screen.
public struct LogEntry: Equatable, Sendable {
    public var timestampUs: Int64
    public var level: LogLevel
    public var category: String
    public var message: String

    public init(timestampUs: Int64, level: LogLevel, category: String, message: String) {
        self.timestampUs = timestampUs
        self.level = level
        self.category = category
        self.message = message
    }

    public var logMessage: LogMessage {
        LogMessage(level: level, category: category, message: message)
    }
}

/// Bounded log buffer: when full, the oldest entry is discarded and counted.
public struct LogRing: Sendable {
    public let capacity: Int
    private var entries = FIFOQueue<LogEntry>()
    public private(set) var droppedCount = 0

    public init(capacity: Int = 1_000) {
        self.capacity = max(1, capacity)
    }

    public var count: Int { entries.count }
    public var isEmpty: Bool { entries.isEmpty }

    public mutating func append(_ entry: LogEntry) {
        entries.append(entry)
        while entries.count > capacity {
            _ = entries.popFirst()
            droppedCount += 1
        }
    }

    /// Removes and returns up to `maxCount` of the oldest entries.
    public mutating func drain(maxCount: Int) -> [LogEntry] {
        var drained: [LogEntry] = []
        while drained.count < maxCount, let entry = entries.popFirst() {
            drained.append(entry)
        }
        return drained
    }

    /// The newest `count` entries, oldest first, without removing them.
    public func newest(_ count: Int) -> [LogEntry] {
        Array(entries.elements.suffix(max(0, count)))
    }

    /// Returns the number of entries dropped since the last call and resets the counter.
    public mutating func takeDroppedCount() -> Int {
        defer { droppedCount = 0 }
        return droppedCount
    }
}
