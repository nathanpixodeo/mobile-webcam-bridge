import Foundation

/// The fixed 24-byte packet header (SPEC §1).
public struct PacketHeader: Equatable, Sendable {
    public static let byteCount = 24
    public static let magic: UInt32 = 0x4D57_4252 // "MWBR"

    public var rawType: UInt8
    public var flags: UInt8
    public var seq: UInt32
    public var payloadLength: UInt32
    public var timestampUs: Int64

    public init(rawType: UInt8, flags: UInt8, seq: UInt32, payloadLength: UInt32, timestampUs: Int64) {
        self.rawType = rawType
        self.flags = flags
        self.seq = seq
        self.payloadLength = payloadLength
        self.timestampUs = timestampUs
    }

    public var type: PacketType? { PacketType(rawValue: rawType) }
}

/// One packet: header fields plus payload. The payload length is derived from `payload`.
public struct Packet: Equatable, Sendable {
    public var rawType: UInt8
    public var flags: UInt8
    public var seq: UInt32
    public var timestampUs: Int64
    public var payload: Data

    public init(rawType: UInt8, flags: UInt8 = 0, seq: UInt32, timestampUs: Int64, payload: Data = Data()) {
        self.rawType = rawType
        self.flags = flags
        self.seq = seq
        self.timestampUs = timestampUs
        self.payload = payload
    }

    public init(type: PacketType, flags: UInt8 = 0, seq: UInt32, timestampUs: Int64, payload: Data = Data()) {
        self.init(rawType: type.rawValue, flags: flags, seq: seq, timestampUs: timestampUs, payload: payload)
    }

    public init(header: PacketHeader, payload: Data) {
        self.init(rawType: header.rawType, flags: header.flags, seq: header.seq,
                  timestampUs: header.timestampUs, payload: payload)
    }

    public var type: PacketType? { PacketType(rawValue: rawType) }

    public var header: PacketHeader {
        PacketHeader(rawType: rawType, flags: flags, seq: seq,
                     payloadLength: UInt32(payload.count), timestampUs: timestampUs)
    }
}

/// Binary encoding of packets.
public enum PacketCodec {
    public static func encode(_ packet: Packet) -> Data {
        var writer = ByteWriter(capacity: PacketHeader.byteCount + packet.payload.count)
        writeHeader(packet.header, into: &writer)
        writer.write(packet.payload)
        return writer.data
    }

    public static func encodeHeader(_ header: PacketHeader) -> Data {
        var writer = ByteWriter(capacity: PacketHeader.byteCount)
        writeHeader(header, into: &writer)
        return writer.data
    }

    /// Decodes the first 24 bytes of `bytes` and validates magic and payload length.
    public static func decodeHeader(_ bytes: Data) throws -> PacketHeader {
        var reader = ByteReader(bytes)
        guard let magic = reader.readUnsigned(UInt32.self),
              let rawType = reader.readUInt8(),
              let flags = reader.readUInt8(),
              reader.readUnsigned(UInt16.self) != nil, // reserved, ignored
              let seq = reader.readUnsigned(UInt32.self),
              let payloadLength = reader.readUnsigned(UInt32.self),
              let timestampUs = reader.readInt64()
        else {
            preconditionFailure("decodeHeader requires at least \(PacketHeader.byteCount) bytes")
        }
        guard magic == PacketHeader.magic else { throw ProtocolError.badMagic(magic) }
        try ProtocolLimits.rule(forRawType: rawType).validate(length: payloadLength, rawType: rawType)
        return PacketHeader(rawType: rawType, flags: flags, seq: seq,
                            payloadLength: payloadLength, timestampUs: timestampUs)
    }

    private static func writeHeader(_ header: PacketHeader, into writer: inout ByteWriter) {
        writer.write(bigEndian: PacketHeader.magic)
        writer.write(header.rawType)
        writer.write(header.flags)
        writer.write(bigEndian: UInt16(0))
        writer.write(bigEndian: header.seq)
        writer.write(bigEndian: header.payloadLength)
        writer.write(bigEndian: header.timestampUs)
    }
}
