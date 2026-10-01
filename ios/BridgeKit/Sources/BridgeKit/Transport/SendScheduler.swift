import Foundation

/// What kind of traffic a packet is; decides its priority and drop policy.
public enum OutboundClass: Equatable, Sendable {
    case control
    case audio
    case video(isKeyframe: Bool, isDisposable: Bool)
    case log
}

/// A packet waiting to be written, with its wire bytes computed once.
public struct OutboundItem: Equatable, Sendable {
    public let kind: OutboundClass
    public let bytes: Data

    public init(packet: Packet, kind: OutboundClass) {
        self.kind = kind
        bytes = PacketCodec.encode(packet)
    }

    var isVideoKeyframe: Bool {
        if case .video(let isKeyframe, _) = kind { return isKeyframe }
        return false
    }

    var isDisposableVideo: Bool {
        if case .video(let isKeyframe, let isDisposable) = kind { return isDisposable && !isKeyframe }
        return false
    }
}

/// Orders outgoing traffic and protects latency under congestion (plan §3, SPEC §3.4).
///
/// - Priority: control > audio > video > log.
/// - At most `windowBytes` are handed to the connection and not yet confirmed as processed.
/// - Video is the only traffic that is shed: disposable frames first, then everything up to the
///   next IDR, at which point a keyframe is requested.
/// - Audio is never dropped; more than one second of queued audio is reported as fatal.
/// - Logs are bounded and the oldest are dropped.
public struct SendScheduler: Sendable {
    public struct Configuration: Equatable, Sendable {
        public var windowBytes: Int
        public var maxQueuedVideoFrames: Int
        public var maxQueuedAudioBytes: Int
        public var maxQueuedLogs: Int

        public init(windowBytes: Int = 256 * 1024, maxQueuedVideoFrames: Int = 3,
                    maxQueuedAudioBytes: Int = AudioChunk.sampleRate * AudioChunk.bytesPerSample,
                    maxQueuedLogs: Int = 200) {
            self.windowBytes = windowBytes
            self.maxQueuedVideoFrames = maxQueuedVideoFrames
            self.maxQueuedAudioBytes = maxQueuedAudioBytes
            self.maxQueuedLogs = maxQueuedLogs
        }
    }

    public struct EnqueueResult: Equatable, Sendable {
        public var droppedVideoFrames = 0
        public var droppedLogs = 0
        /// The encoder must produce an IDR: frames were dropped and decoding can only resume there.
        public var keyframeNeeded = false
        /// More audio is queued than the connection can ever catch up with; close it.
        public var audioBacklogExceeded = false

        public init() {}
    }

    public let configuration: Configuration
    public private(set) var inFlightBytes = 0
    public private(set) var queuedAudioBytes = 0
    private var control = FIFOQueue<OutboundItem>()
    private var audio = FIFOQueue<OutboundItem>()
    private var video = FIFOQueue<OutboundItem>()
    private var logs = FIFOQueue<OutboundItem>()
    private var droppingUntilKeyframe = false

    public init(configuration: Configuration = Configuration()) {
        self.configuration = configuration
    }

    public var queuedVideoFrames: Int { video.count }
    public var queuedLogCount: Int { logs.count }
    public var isIdle: Bool { control.isEmpty && audio.isEmpty && video.isEmpty && logs.isEmpty }

    public mutating func enqueue(_ item: OutboundItem) -> EnqueueResult {
        var result = EnqueueResult()
        switch item.kind {
        case .control:
            control.append(item)
        case .audio:
            audio.append(item)
            queuedAudioBytes += item.bytes.count
            result.audioBacklogExceeded = queuedAudioBytes > configuration.maxQueuedAudioBytes
        case .video:
            enqueueVideo(item, into: &result)
        case .log:
            logs.append(item)
            while logs.count > configuration.maxQueuedLogs {
                _ = logs.popFirst()
                result.droppedLogs += 1
            }
        }
        return result
    }

    /// Next item to write, or nil when nothing is queued or the window is full. A single item
    /// larger than the window is still sent once nothing else is in flight.
    public mutating func dequeue() -> OutboundItem? {
        guard let next = peekNext() else { return nil }
        guard inFlightBytes == 0 || inFlightBytes + next.bytes.count <= configuration.windowBytes else { return nil }
        let item = popNext()
        inFlightBytes += item.bytes.count
        return item
    }

    /// The connection finished processing `bytes` previously returned by `dequeue()`.
    public mutating func acknowledge(bytes: Int) {
        inFlightBytes = max(0, inFlightBytes - bytes)
    }

    /// Drops queued video, used when a new `VideoConfig` makes older frames meaningless.
    public mutating func purgeVideo() {
        video.removeAll()
        droppingUntilKeyframe = false
    }

    /// Forgets everything, for a new connection.
    public mutating func reset() {
        control.removeAll()
        audio.removeAll()
        video.removeAll()
        logs.removeAll()
        inFlightBytes = 0
        queuedAudioBytes = 0
        droppingUntilKeyframe = false
    }

    // MARK: - Private

    private mutating func enqueueVideo(_ item: OutboundItem, into result: inout EnqueueResult) {
        if droppingUntilKeyframe {
            guard item.isVideoKeyframe else {
                result.droppedVideoFrames += 1
                return
            }
            droppingUntilKeyframe = false
        }
        video.append(item)
        guard video.count > configuration.maxQueuedVideoFrames else { return }

        // 1. Disposable frames are never referenced: shedding them costs nothing.
        var excess = video.count - configuration.maxQueuedVideoFrames
        let removedDisposable = video.removeAll { candidate in
            guard excess > 0, candidate.isDisposableVideo else { return false }
            excess -= 1
            return true
        }
        result.droppedVideoFrames += removedDisposable.count
        guard video.count > configuration.maxQueuedVideoFrames else { return }

        // 2. Dropping a reference frame breaks every later frame until the next IDR.
        if item.isVideoKeyframe {
            // The new IDR (appended last) supersedes everything queued before it.
            result.droppedVideoFrames += video.count - 1
            video.removeAll()
            video.append(item)
        } else {
            result.droppedVideoFrames += video.count
            video.removeAll()
            droppingUntilKeyframe = true
            result.keyframeNeeded = true
        }
    }

    private func peekNext() -> OutboundItem? {
        control.first ?? audio.first ?? video.first ?? logs.first
    }

    private mutating func popNext() -> OutboundItem {
        if let item = control.popFirst() { return item }
        if let item = audio.popFirst() {
            queuedAudioBytes -= item.bytes.count
            return item
        }
        if let item = video.popFirst() { return item }
        if let item = logs.popFirst() { return item }
        preconditionFailure("popNext called with empty queues")
    }
}
