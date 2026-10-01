import Foundation

/// One encoded H.264 access unit, already in Annex-B form, ready to cross queues.
public struct EncodedVideoFrame: Equatable, Sendable {
    /// Identifies the `VideoConfig` this frame belongs to; stale frames are dropped.
    public var configId: Int
    public var annexB: Data
    public var isKeyframe: Bool
    public var hasParameterSets: Bool
    /// Not referenced by any other frame, so it can be dropped without breaking decoding.
    public var isDisposable: Bool
    /// Presentation time on the host-time clock, in microseconds.
    public var presentationTimeUs: Int64

    public init(configId: Int, annexB: Data, isKeyframe: Bool, hasParameterSets: Bool,
                isDisposable: Bool, presentationTimeUs: Int64) {
        self.configId = configId
        self.annexB = annexB
        self.isKeyframe = isKeyframe
        self.hasParameterSets = hasParameterSets
        self.isDisposable = isDisposable
        self.presentationTimeUs = presentationTimeUs
    }

    public var flags: VideoAccessUnitFlags {
        var flags: VideoAccessUnitFlags = []
        if isKeyframe { flags.insert(.idr) }
        if hasParameterSets { flags.insert(.parameterSets) }
        if isDisposable { flags.insert(.disposable) }
        return flags
    }
}

/// A chunk of PCM `s16le`, 48 kHz, mono (SPEC §3.4).
public struct AudioChunk: Equatable, Sendable {
    public static let sampleRate = 48_000
    public static let bytesPerSample = 2
    /// 40 ms, the largest chunk the spec allows.
    public static let maxSamplesPerChunk = 1_920

    public var configId: Int
    public var pcm: Data
    /// Capture time of the first sample on the host-time clock, in microseconds.
    public var timestampUs: Int64
    public var isDiscontinuity: Bool

    public init(configId: Int, pcm: Data, timestampUs: Int64, isDiscontinuity: Bool) {
        self.configId = configId
        self.pcm = pcm
        self.timestampUs = timestampUs
        self.isDiscontinuity = isDiscontinuity
    }

    public var sampleCount: Int { pcm.count / Self.bytesPerSample }

    public var durationUs: Int64 { Int64(sampleCount) * 1_000_000 / Int64(Self.sampleRate) }

    /// Splits PCM into chunks of at most 40 ms; only the first keeps the discontinuity flag.
    public static func split(pcm: Data, configId: Int, timestampUs: Int64, isDiscontinuity: Bool) -> [AudioChunk] {
        let maxBytes = maxSamplesPerChunk * bytesPerSample
        var chunks: [AudioChunk] = []
        var offset = pcm.startIndex
        var samplesBefore: Int64 = 0
        while offset < pcm.endIndex {
            let end = min(offset + maxBytes, pcm.endIndex)
            let slice = Data(pcm[offset..<end])
            let timestamp = timestampUs + samplesBefore * 1_000_000 / Int64(sampleRate)
            chunks.append(AudioChunk(configId: configId, pcm: slice, timestampUs: timestamp,
                                     isDiscontinuity: isDiscontinuity && chunks.isEmpty))
            samplesBefore += Int64(slice.count / bytesPerSample)
            offset = end
        }
        return chunks
    }
}
