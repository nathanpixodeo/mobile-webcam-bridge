import Foundation

/// Incremental parser: feed it TCP chunks of any size, get back complete packets.
///
/// The header is validated as soon as its 24 bytes are available, so an oversized length is
/// rejected before any payload is buffered. After the first error the parser stays failed:
/// the connection must be closed.
public struct PacketStreamParser: Sendable {
    private var buffer = Data()
    private var pendingHeader: PacketHeader?
    private var failure: ProtocolError?

    public init() {}

    /// Bytes received but not yet returned as part of a packet.
    public var bufferedByteCount: Int { buffer.count }

    public mutating func push(_ chunk: Data) throws -> [Packet] {
        if let failure { throw failure }
        buffer.append(chunk)

        var packets: [Packet] = []
        var cursor = buffer.startIndex
        do {
            while true {
                if pendingHeader == nil {
                    guard buffer.endIndex - cursor >= PacketHeader.byteCount else { break }
                    pendingHeader = try PacketCodec.decodeHeader(buffer[cursor..<(cursor + PacketHeader.byteCount)])
                    cursor += PacketHeader.byteCount
                }
                guard let header = pendingHeader else { break }
                let length = Int(header.payloadLength)
                guard buffer.endIndex - cursor >= length else { break }
                // Copy so the payload's indices start at zero and the buffer can be compacted.
                let payload = Data(buffer[cursor..<(cursor + length)])
                cursor += length
                packets.append(Packet(header: header, payload: payload))
                pendingHeader = nil
            }
        } catch let error as ProtocolError {
            failure = error
            buffer = Data()
            pendingHeader = nil
            throw error
        }

        buffer.removeSubrange(buffer.startIndex..<cursor)
        return packets
    }
}
