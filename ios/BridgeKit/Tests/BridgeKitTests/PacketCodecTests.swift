import Foundation
import XCTest
@testable import BridgeKit

final class PacketCodecTests: XCTestCase {
    func testValidVectorsDecodeAndReencode() throws {
        let vectors = try PacketVectors.load()
        XCTAssertEqual(vectors.headerSize, PacketHeader.byteCount)

        for vector in vectors.valid {
            let bytes = Data(hex: vector.hex)
            let header = try PacketCodec.decodeHeader(bytes)
            XCTAssertEqual(header.rawType, vector.header.type, vector.name)
            XCTAssertEqual(header.flags, vector.header.flags, vector.name)
            XCTAssertEqual(header.seq, vector.header.seq, vector.name)
            XCTAssertEqual(header.payloadLength, vector.header.payloadLength, vector.name)
            XCTAssertEqual(header.timestampUs, Int64(vector.header.timestampUs), vector.name)

            let payload = bytes.subdata(in: PacketHeader.byteCount..<bytes.count)
            let packet = Packet(header: header, payload: payload)
            XCTAssertEqual(PacketCodec.encode(packet).hex, vector.hex, vector.name)
        }
    }

    func testPongPayloadMatchesVector() throws {
        let vector = try PacketVectors.load().vector(named: "pong")
        let packet = try XCTUnwrap(PacketStreamParser().parseAll(Data(hex: vector.hex)).first)
        let pong = try PongPayload(decoding: packet.payload)
        XCTAssertEqual(pong.echoT0, Int64(try XCTUnwrap(vector.payload.echoT0)))
        XCTAssertEqual(pong.t1, Int64(try XCTUnwrap(vector.payload.t1)))
        XCTAssertEqual(pong.encoded(), packet.payload)
    }

    func testFactoryAssignsPerTypeSequenceNumbers() throws {
        var factory = OutboundPacketFactory()
        let first = factory.ping(timestampUs: 1)
        let log = try factory.json(.log, LogMessage(level: .info, category: "a", message: "b"), timestampUs: 2)
        let second = factory.ping(timestampUs: 3)
        XCTAssertEqual(first.seq, 0)
        XCTAssertEqual(log.seq, 0)
        XCTAssertEqual(second.seq, 1)
    }

    func testFactoryRejectsOversizedLog() {
        var factory = OutboundPacketFactory()
        let message = LogMessage(level: .info, category: "c", message: String(repeating: "x", count: 9_000))
        XCTAssertThrowsError(try factory.json(.log, message, timestampUs: 0)) { error in
            XCTAssertEqual(error as? OutboundPacketFactory.BuildError,
                           .payloadTooLarge(type: .log, length: 9_000 + 44))
        }
        XCTAssertNoThrow(try factory.json(.log, message.truncated(), timestampUs: 0))
    }

    func testVideoAccessUnitFlags() throws {
        var factory = OutboundPacketFactory()
        let frame = EncodedVideoFrame(configId: 1, annexB: Data([0, 0, 0, 1, 0x65]), isKeyframe: true,
                                      hasParameterSets: true, isDisposable: false, presentationTimeUs: 42)
        let packet = try factory.videoAccessUnit(frame)
        XCTAssertEqual(packet.flags, 0x03)
        XCTAssertEqual(packet.timestampUs, 42)
    }
}

extension PacketStreamParser {
    /// Feeds everything at once; test convenience.
    func parseAll(_ data: Data) throws -> [Packet] {
        var parser = self
        return try parser.push(data)
    }
}
