import Foundation

/// A packet from the host, decoded into what the device session acts on.
public enum HostCommand: Equatable, Sendable {
    case hello(HelloMessage)
    case error(ErrorMessage)
    case ping(t0: Int64)
    case pong(PongPayload, sentAtUs: Int64)
    case startVideo(StartVideoMessage)
    case stopVideo
    case requestKeyframe
    case startAudio(StartAudioMessage)
    case stopAudio
    /// Device-to-host types echoed back, or types this version does not know: skipped.
    case ignored(rawType: UInt8)
}

/// A packet whose framing was valid but whose content was not.
public enum MessageDecodingError: Error, Equatable, Sendable {
    case invalidJSON(type: PacketType, detail: String)
    case invalidValue(type: PacketType, detail: String)

    public var type: PacketType {
        switch self {
        case .invalidJSON(let type, _), .invalidValue(let type, _): type
        }
    }

    public var detail: String {
        switch self {
        case .invalidJSON(_, let detail), .invalidValue(_, let detail): detail
        }
    }
}

public enum HostCommandDecoder {
    public static func decode(_ packet: Packet) throws -> HostCommand {
        guard let type = packet.type else { return .ignored(rawType: packet.rawType) }
        switch type {
        case .hello:
            return .hello(try json(HelloMessage.self, packet, type))
        case .error:
            return .error(try json(ErrorMessage.self, packet, type))
        case .ping:
            return .ping(t0: packet.timestampUs)
        case .pong:
            return .pong(try PongPayload(decoding: packet.payload), sentAtUs: packet.timestampUs)
        case .startVideo:
            let message = try json(StartVideoMessage.self, packet, type)
            try message.validate()
            return .startVideo(message)
        case .stopVideo:
            return .stopVideo
        case .requestKeyframe:
            return .requestKeyframe
        case .startAudio:
            return .startAudio(try json(StartAudioMessage.self, packet, type))
        case .stopAudio:
            return .stopAudio
        case .videoConfig, .videoAccessUnit, .audioConfig, .audioChunk, .status, .log:
            return .ignored(rawType: packet.rawType)
        }
    }

    private static func json<T: Decodable>(_: T.Type, _ packet: Packet, _ type: PacketType) throws -> T {
        do {
            return try JSONDecoder().decode(T.self, from: packet.payload)
        } catch {
            throw MessageDecodingError.invalidJSON(type: type, detail: String(describing: error))
        }
    }
}
