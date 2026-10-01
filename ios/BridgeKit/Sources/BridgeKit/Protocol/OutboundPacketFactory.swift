import Foundation

/// Builds outgoing packets and assigns the per-type `seq` (SPEC §1).
///
/// Sequence numbers are assigned when a packet is produced, before the send scheduler may drop
/// it, so the host sees every deliberate drop as a gap.
public struct OutboundPacketFactory: Sendable {
    public enum BuildError: Error, Equatable, Sendable {
        case payloadTooLarge(type: PacketType, length: Int)
    }

    private var nextSeq: [UInt8: UInt32] = [:]

    public init() {}

    public mutating func packet(_ type: PacketType, flags: UInt8 = 0, timestampUs: Int64,
                                payload: Data = Data()) throws -> Packet {
        try ProtocolLimits.rule(forRawType: type.rawValue)
            .validate(length: UInt32(clamping: payload.count), rawType: type.rawValue)
        return Packet(type: type, flags: flags, seq: takeSeq(for: type), timestampUs: timestampUs, payload: payload)
    }

    public mutating func json<Message: Encodable>(_ type: PacketType, _ message: Message,
                                                  timestampUs: Int64) throws -> Packet {
        let payload = try CanonicalJSON.encode(message)
        do {
            return try packet(type, timestampUs: timestampUs, payload: payload)
        } catch is ProtocolError {
            throw BuildError.payloadTooLarge(type: type, length: payload.count)
        }
    }

    public mutating func ping(timestampUs: Int64) -> Packet {
        Packet(type: .ping, seq: takeSeq(for: .ping), timestampUs: timestampUs)
    }

    public mutating func pong(_ payload: PongPayload, timestampUs: Int64) -> Packet {
        Packet(type: .pong, seq: takeSeq(for: .pong), timestampUs: timestampUs, payload: payload.encoded())
    }

    public mutating func videoAccessUnit(_ frame: EncodedVideoFrame) throws -> Packet {
        try packet(.videoAccessUnit, flags: frame.flags.rawValue, timestampUs: frame.presentationTimeUs,
                   payload: frame.annexB)
    }

    public mutating func audioChunk(_ chunk: AudioChunk) throws -> Packet {
        let flags: AudioChunkFlags = chunk.isDiscontinuity ? .discontinuity : []
        return try packet(.audioChunk, flags: flags.rawValue, timestampUs: chunk.timestampUs, payload: chunk.pcm)
    }

    private mutating func takeSeq(for type: PacketType) -> UInt32 {
        let seq = nextSeq[type.rawValue, default: 0]
        nextSeq[type.rawValue] = seq &+ 1
        return seq
    }
}
