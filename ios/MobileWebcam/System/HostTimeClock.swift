import BridgeKit
import CoreMedia

/// Microseconds on the host-time clock (mach_absolute_time). Packet timestamps and media
/// timestamps both use this clock, so the host can map them with one offset (SPEC §3.2).
struct HostTimeClock: MonotonicClock {
    func nowMicroseconds() -> Int64 {
        Self.microseconds(CMClockGetTime(CMClockGetHostTimeClock()))
    }

    static func microseconds(_ time: CMTime) -> Int64 {
        CMTimeConvertScale(time, timescale: 1_000_000, method: .roundTowardZero).value
    }

    /// Converts a capture timestamp from the capture session's synchronization clock to host time.
    static func microseconds(_ time: CMTime, from clock: CMClock?) -> Int64 {
        guard let clock else { return microseconds(time) }
        return microseconds(CMSyncConvertTime(time, from: clock, to: CMClockGetHostTimeClock()))
    }
}
