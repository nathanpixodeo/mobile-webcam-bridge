/// One Ping/Pong exchange, evaluated NTP style (SPEC §3.1).
public struct ClockSample: Equatable, Sendable {
    public let roundTripUs: Int64
    /// Peer clock minus local clock.
    public let offsetUs: Int64

    public init(roundTripUs: Int64, offsetUs: Int64) {
        self.roundTripUs = roundTripUs
        self.offsetUs = offsetUs
    }
}

/// Keeps the last `windowSize` samples and trusts the one with the lowest round trip, which is
/// the least disturbed by queuing delay.
public struct ClockOffsetEstimator: Sendable {
    public let windowSize: Int
    private var samples: [ClockSample] = []

    public init(windowSize: Int = 16) {
        self.windowSize = max(1, windowSize)
    }

    /// `t0` local send, `t1` peer receive, `t2` peer send, `t3` local receive.
    @discardableResult
    public mutating func add(t0: Int64, t1: Int64, t2: Int64, t3: Int64) -> ClockSample? {
        let roundTrip = (t3 - t0) - (t2 - t1)
        guard roundTrip >= 0 else { return nil }
        let sample = ClockSample(roundTripUs: roundTrip, offsetUs: ((t1 - t0) + (t2 - t3)) / 2)
        samples.append(sample)
        if samples.count > windowSize { samples.removeFirst(samples.count - windowSize) }
        return sample
    }

    public var best: ClockSample? {
        samples.min { $0.roundTripUs < $1.roundTripUs }
    }

    public mutating func reset() {
        samples.removeAll()
    }
}
