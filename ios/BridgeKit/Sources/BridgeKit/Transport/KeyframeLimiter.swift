/// Coalesces keyframe requests (host `RequestKeyframe`, scheduler drops) into at most one
/// forced IDR per interval — 2 per second by default (SPEC §3.3).
///
/// Requests only mark a keyframe as pending; the encoder asks once per frame whether to force it.
public struct KeyframeLimiter: Sendable {
    public let minIntervalUs: Int64
    private var pending = false
    private var lastKeyframeUs: Int64?

    public init(minIntervalUs: Int64 = 500_000) {
        self.minIntervalUs = minIntervalUs
    }

    public var isPending: Bool { pending }

    public mutating func request() {
        pending = true
    }

    /// Call for every frame about to be encoded. Returns true when this frame must be an IDR.
    public mutating func shouldForceKeyframe(nowUs: Int64) -> Bool {
        guard pending else { return false }
        if let last = lastKeyframeUs, nowUs - last < minIntervalUs { return false }
        pending = false
        lastKeyframeUs = nowUs
        return true
    }

    /// Records a keyframe the encoder produced on its own (first frame, encoder GOP).
    public mutating func noteKeyframe(atUs nowUs: Int64) {
        pending = false
        lastKeyframeUs = nowUs
    }

    public mutating func reset() {
        pending = false
        lastKeyframeUs = nil
    }
}
