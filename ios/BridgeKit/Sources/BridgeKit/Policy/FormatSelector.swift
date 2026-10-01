/// A capture format described with plain values, so selection logic is testable without
/// AVFoundation. Dimensions are in the sensor's native landscape orientation.
public struct CaptureFormatInfo: Equatable, Sendable {
    public var index: Int
    public var width: Int
    public var height: Int
    public var minFrameRate: Double
    public var maxFrameRate: Double
    /// 4:2:0 bi-planar video range (`420v`), the format the encoder is fed with.
    public var isVideoRange420: Bool

    public init(index: Int, width: Int, height: Int, minFrameRate: Double, maxFrameRate: Double,
                isVideoRange420: Bool) {
        self.index = index
        self.width = width
        self.height = height
        self.minFrameRate = minFrameRate
        self.maxFrameRate = maxFrameRate
        self.isVideoRange420 = isVideoRange420
    }

    var area: Int { width * height }

    func supports(fps: Int) -> Bool {
        let rate = Double(fps)
        return minFrameRate <= rate + 0.01 && maxFrameRate + 0.01 >= rate
    }
}

/// Chooses the capture format for a requested size and frame rate.
///
/// Preference order: exact size; then the smallest larger format with the same aspect ratio;
/// then the smallest format covering the requested size; then the largest available. Among
/// equals, the lowest maximum frame rate wins (it is usually the lower-power mode).
public struct FormatSelector: Sendable {
    public init() {}

    public func select(from formats: [CaptureFormatInfo], width: Int, height: Int, fps: Int) -> CaptureFormatInfo? {
        let videoRange = formats.filter(\.isVideoRange420)
        let pool = videoRange.isEmpty ? formats : videoRange
        let fpsCapable = pool.filter { $0.supports(fps: fps) }
        let candidates = fpsCapable.isEmpty ? pool : fpsCapable
        guard !candidates.isEmpty else { return nil }

        let exact = candidates.filter { $0.width == width && $0.height == height }
        if let best = cheapest(exact) { return best }

        let sameAspectLarger = candidates.filter {
            $0.width * height == $0.height * width && $0.width >= width && $0.height >= height
        }
        if let best = smallest(sameAspectLarger) { return best }

        let covering = candidates.filter { $0.width >= width && $0.height >= height }
        if let best = smallest(covering) { return best }

        return candidates.max { lhs, rhs in
            lhs.area != rhs.area ? lhs.area < rhs.area : lhs.maxFrameRate > rhs.maxFrameRate
        }
    }

    private func cheapest(_ formats: [CaptureFormatInfo]) -> CaptureFormatInfo? {
        formats.min { lhs, rhs in
            lhs.maxFrameRate != rhs.maxFrameRate ? lhs.maxFrameRate < rhs.maxFrameRate : lhs.index < rhs.index
        }
    }

    private func smallest(_ formats: [CaptureFormatInfo]) -> CaptureFormatInfo? {
        formats.min { lhs, rhs in
            if lhs.area != rhs.area { return lhs.area < rhs.area }
            if lhs.maxFrameRate != rhs.maxFrameRate { return lhs.maxFrameRate < rhs.maxFrameRate }
            return lhs.index < rhs.index
        }
    }
}
