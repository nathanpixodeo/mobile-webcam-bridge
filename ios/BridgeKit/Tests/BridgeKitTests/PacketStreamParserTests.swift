import Foundation
import XCTest
@testable import BridgeKit

final class PacketStreamParserTests: XCTestCase {
    func testStreamsYieldExpectedPackets() throws {
        for stream in try PacketVectors.load().streams {
            let packets = try PacketStreamParser().parseAll(Data(hex: stream.hex))
            assertPackets(packets, match: stream.expect, stream.name)
        }
    }

    func testEverySplitPointYieldsTheSamePackets() throws {
        let vectors = try PacketVectors.load()
        let inputs = vectors.streams.map { ($0.name, Data(hex: $0.hex), $0.expect) }
        for (name, bytes, expected) in inputs {
            for split in 0...bytes.count {
                var parser = PacketStreamParser()
                var packets = try parser.push(bytes.subdata(in: 0..<split))
                packets += try parser.push(bytes.subdata(in: split..<bytes.count))
                assertPackets(packets, match: expected, "\(name) split at \(split)")
                XCTAssertEqual(parser.bufferedByteCount, 0)
            }
        }
    }

    func testByteByByteFeeding() throws {
        for stream in try PacketVectors.load().streams {
            var parser = PacketStreamParser()
            var packets: [Packet] = []
            for byte in Data(hex: stream.hex) {
                packets += try parser.push(Data([byte]))
            }
            assertPackets(packets, match: stream.expect, stream.name)
        }
    }

    func testSeededRandomChunking() throws {
        var generator = SplitMix64(seed: 0x1BAD_C0DE)
        for stream in try PacketVectors.load().streams {
            let bytes = Data(hex: stream.hex)
            for _ in 0..<50 {
                var parser = PacketStreamParser()
                var packets: [Packet] = []
                var offset = 0
                while offset < bytes.count {
                    let size = Int(generator.next() % 17) + 1
                    let end = min(offset + size, bytes.count)
                    packets += try parser.push(bytes.subdata(in: offset..<end))
                    offset = end
                }
                assertPackets(packets, match: stream.expect, stream.name)
            }
        }
    }

    func testInvalidVectorsFailWithExpectedCode() throws {
        for vector in try PacketVectors.load().invalid {
            var parser = PacketStreamParser()
            XCTAssertThrowsError(try parser.push(Data(hex: vector.hex)), vector.name) { error in
                XCTAssertEqual((error as? ProtocolError)?.code, vector.error, vector.name)
            }
        }
    }

    func testParserStaysFailedAfterAnError() throws {
        let vectors = try PacketVectors.load()
        let bad = try XCTUnwrap(vectors.invalid.first { $0.error == "BAD_MAGIC" })
        let ping = try vectors.vector(named: "ping")
        var parser = PacketStreamParser()
        XCTAssertThrowsError(try parser.push(Data(hex: bad.hex)))
        XCTAssertThrowsError(try parser.push(Data(hex: ping.hex)))
    }

    func testOversizedHeaderIsRejectedBeforePayloadArrives() throws {
        let vector = try XCTUnwrap(PacketVectors.load().invalid.first { $0.name == "video-too-large" })
        var parser = PacketStreamParser()
        // Only the header is present; the error must not wait for 4 MiB of payload.
        XCTAssertThrowsError(try parser.push(Data(hex: vector.hex)))
    }

    private func assertPackets(_ packets: [Packet], match expected: [PacketVectors.ExpectedPacket], _ message: String,
                        file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertEqual(packets.count, expected.count, message, file: file, line: line)
        for (packet, expectation) in zip(packets, expected) {
            XCTAssertEqual(packet.rawType, expectation.type, message, file: file, line: line)
            XCTAssertEqual(packet.flags, expectation.flags, message, file: file, line: line)
            XCTAssertEqual(packet.seq, expectation.seq, message, file: file, line: line)
            XCTAssertEqual(packet.payload.hex, expectation.payloadHex, message, file: file, line: line)
        }
    }
}

/// Small deterministic PRNG so fuzz runs are reproducible on every platform.
struct SplitMix64 {
    private var state: UInt64

    init(seed: UInt64) {
        state = seed
    }

    mutating func next() -> UInt64 {
        state &+= 0x9E37_79B9_7F4A_7C15
        var z = state
        z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
        z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
        return z ^ (z >> 31)
    }
}
