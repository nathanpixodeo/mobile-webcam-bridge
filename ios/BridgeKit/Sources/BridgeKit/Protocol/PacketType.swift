/// Packet type codes, protocol/SPEC.md §3.
public enum PacketType: UInt8, CaseIterable, Sendable {
    case hello = 0x01
    case error = 0x02
    case ping = 0x03
    case pong = 0x04
    case startVideo = 0x10
    case stopVideo = 0x11
    case requestKeyframe = 0x12
    case startAudio = 0x13
    case stopAudio = 0x14
    case videoConfig = 0x20
    case videoAccessUnit = 0x21
    case audioConfig = 0x30
    case audioChunk = 0x31
    case status = 0x40
    case log = 0x41
}

/// Flags of `VideoAccessUnit` packets.
public struct VideoAccessUnitFlags: OptionSet, Sendable {
    public let rawValue: UInt8

    public init(rawValue: UInt8) {
        self.rawValue = rawValue
    }

    public static let idr = VideoAccessUnitFlags(rawValue: 0x01)
    public static let parameterSets = VideoAccessUnitFlags(rawValue: 0x02)
    public static let disposable = VideoAccessUnitFlags(rawValue: 0x04)
}

/// Flags of `AudioChunk` packets.
public struct AudioChunkFlags: OptionSet, Sendable {
    public let rawValue: UInt8

    public init(rawValue: UInt8) {
        self.rawValue = rawValue
    }

    public static let discontinuity = AudioChunkFlags(rawValue: 0x01)
}
