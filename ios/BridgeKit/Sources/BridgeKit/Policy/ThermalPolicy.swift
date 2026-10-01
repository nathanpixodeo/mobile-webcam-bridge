/// Platform-neutral thermal level: the worse of the process thermal state and the camera's
/// system pressure level.
public enum ThermalLevel: Int, Comparable, CaseIterable, Sendable {
    case nominal
    case fair
    case serious
    case critical
    case shutdown

    public static func < (lhs: ThermalLevel, rhs: ThermalLevel) -> Bool { lhs.rawValue < rhs.rawValue }

    /// Name reported in `Status.thermal`; the wire format has no `shutdown`.
    public var statusName: ThermalStateName {
        switch self {
        case .nominal: .nominal
        case .fair: .fair
        case .serious: .serious
        case .critical, .shutdown: .critical
        }
    }
}

/// Video limits imposed by a thermal level.
public struct VideoThrottle: Equatable, Sendable {
    public var maxFps: Int?
    /// Bitrate multiplier in per mille (1000 = unchanged).
    public var bitratePermille: Int
    public var videoAllowed: Bool

    public init(maxFps: Int?, bitratePermille: Int, videoAllowed: Bool) {
        self.maxFps = maxFps
        self.bitratePermille = bitratePermille
        self.videoAllowed = videoAllowed
    }

    public static let unrestricted = VideoThrottle(maxFps: nil, bitratePermille: 1_000, videoAllowed: true)
}

/// Thermal throttling table from the plan: serious → 24 fps and 70 % bitrate, critical → 15 fps
/// and 50 %, shutdown → no video.
public struct ThermalPolicy: Sendable {
    public init() {}

    public func throttle(for level: ThermalLevel) -> VideoThrottle {
        switch level {
        case .nominal, .fair: .unrestricted
        case .serious: VideoThrottle(maxFps: 24, bitratePermille: 700, videoAllowed: true)
        case .critical: VideoThrottle(maxFps: 15, bitratePermille: 500, videoAllowed: true)
        case .shutdown: VideoThrottle(maxFps: nil, bitratePermille: 0, videoAllowed: false)
        }
    }

    /// Effective frame rate and bitrate after throttling.
    public func apply(_ throttle: VideoThrottle, fps: Int, bitrateKbps: Int) -> (fps: Int, bitrateKbps: Int) {
        let cappedFps = throttle.maxFps.map { min(fps, $0) } ?? fps
        let scaledBitrate = bitrateKbps * throttle.bitratePermille / 1_000
        return (max(1, cappedFps), max(250, scaledBitrate))
    }
}
