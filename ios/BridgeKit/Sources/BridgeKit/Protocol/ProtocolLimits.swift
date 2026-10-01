/// Error raised by the packet layer. Every case is fatal: the connection must be closed,
/// because there is no way to resynchronise a corrupted stream (SPEC §1.1).
public enum ProtocolError: Error, Equatable, Sendable {
    case badMagic(UInt32)
    case payloadTooLarge(rawType: UInt8, length: UInt32)
    case badPayloadLength(rawType: UInt8, length: UInt32)

    /// Stable code shared with the host implementation and the golden test vectors.
    public var code: String {
        switch self {
        case .badMagic: "BAD_MAGIC"
        case .payloadTooLarge: "PAYLOAD_TOO_LARGE"
        case .badPayloadLength: "BAD_PAYLOAD_LENGTH"
        }
    }
}

/// Allowed payload length for one packet type.
public enum PayloadLengthRule: Equatable, Sendable {
    case exactly(UInt32)
    case atMost(UInt32)

    func validate(length: UInt32, rawType: UInt8) throws {
        switch self {
        case .exactly(let expected) where length != expected:
            throw ProtocolError.badPayloadLength(rawType: rawType, length: length)
        case .atMost(let maximum) where length > maximum:
            throw ProtocolError.payloadTooLarge(rawType: rawType, length: length)
        default:
            return
        }
    }
}

/// Payload limits from SPEC §1.1.
public enum ProtocolLimits {
    public static let maxJSONPayload: UInt32 = 65_536
    public static let maxLogPayload: UInt32 = 8_192
    public static let maxAudioPayload: UInt32 = 65_536
    public static let maxVideoPayload: UInt32 = 4_194_304
    public static let maxUnknownPayload: UInt32 = 4_194_304
    public static let pongPayloadLength: UInt32 = 16

    public static func rule(forRawType rawType: UInt8) -> PayloadLengthRule {
        guard let type = PacketType(rawValue: rawType) else { return .atMost(maxUnknownPayload) }
        switch type {
        case .hello, .error, .startVideo, .startAudio, .videoConfig, .audioConfig, .status:
            return .atMost(maxJSONPayload)
        case .log:
            return .atMost(maxLogPayload)
        case .ping, .stopVideo, .requestKeyframe, .stopAudio:
            return .exactly(0)
        case .pong:
            return .exactly(pongPayloadLength)
        case .videoAccessUnit:
            return .atMost(maxVideoPayload)
        case .audioChunk:
            return .atMost(maxAudioPayload)
        }
    }
}
