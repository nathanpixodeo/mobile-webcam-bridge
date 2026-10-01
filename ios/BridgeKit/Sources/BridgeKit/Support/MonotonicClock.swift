import Dispatch

/// Microseconds on a monotonic clock. The app injects a host-time clock (the clock capture
/// timestamps are converted to); tests inject a manual clock.
public protocol MonotonicClock: Sendable {
    func nowMicroseconds() -> Int64
}

/// Monotonic clock backed by `DispatchTime` (mach_absolute_time on Apple platforms).
public struct DispatchMonotonicClock: MonotonicClock {
    public init() {}

    public func nowMicroseconds() -> Int64 {
        Int64(DispatchTime.now().uptimeNanoseconds / 1_000)
    }
}
