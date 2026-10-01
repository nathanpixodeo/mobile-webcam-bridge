import Foundation
import XCTest

/// Loads the golden vectors shared with the host implementation (protocol/test-vectors).
/// SwiftPM resources cannot live outside the package, so the path is derived from this file.
enum TestVectors {
    static var directory: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // Support
            .deletingLastPathComponent() // BridgeKitTests
            .deletingLastPathComponent() // Tests
            .deletingLastPathComponent() // BridgeKit
            .deletingLastPathComponent() // ios
            .appendingPathComponent("protocol")
            .appendingPathComponent("test-vectors")
    }

    static func load<T: Decodable>(_ type: T.Type, _ name: String) throws -> T {
        let url = directory.appendingPathComponent(name)
        return try JSONDecoder().decode(T.self, from: Data(contentsOf: url))
    }
}

struct PacketVectors: Decodable {
    struct Header: Decodable {
        let type: UInt8
        let flags: UInt8
        let seq: UInt32
        let payloadLength: UInt32
        let timestampUs: String
    }

    struct Payload: Decodable {
        let kind: String
        let text: String?
        let hex: String?
        let echoT0: String?
        let t1: String?
    }

    struct Valid: Decodable {
        let name: String
        let hex: String
        let header: Header
        let payload: Payload
    }

    struct Invalid: Decodable {
        let name: String
        let hex: String
        let error: String
    }

    struct ExpectedPacket: Decodable {
        let type: UInt8
        let flags: UInt8
        let seq: UInt32
        let payloadHex: String
    }

    struct Stream: Decodable {
        let name: String
        let hex: String
        let expect: [ExpectedPacket]
    }

    let headerSize: Int
    let valid: [Valid]
    let invalid: [Invalid]
    let streams: [Stream]

    static func load() throws -> PacketVectors {
        try TestVectors.load(PacketVectors.self, "packets.json")
    }

    func vector(named name: String) throws -> Valid {
        try XCTUnwrap(valid.first { $0.name == name }, "missing vector \(name)")
    }
}

struct H264Vectors: Decodable {
    struct NAL: Decodable {
        let type: UInt8
        let hex: String
    }

    struct Split: Decodable {
        let name: String
        let annexB: String
        let nals: [NAL]
        let isIdr: Bool
        let hasParameterSets: Bool
    }

    struct AVCC: Decodable {
        let name: String
        let avcc: String
        let annexB: String
        let sps: String?
        let pps: String?
    }

    let accessUnitDelimiter: String
    let split: [Split]
    let avccToAnnexB: [AVCC]

    static func load() throws -> H264Vectors {
        try TestVectors.load(H264Vectors.self, "h264.json")
    }
}

extension Data {
    init(hex: String) {
        var bytes: [UInt8] = []
        bytes.reserveCapacity(hex.count / 2)
        var iterator = hex.unicodeScalars.makeIterator()
        while let high = iterator.next(), let low = iterator.next() {
            guard let value = UInt8(String(high) + String(low), radix: 16) else {
                preconditionFailure("invalid hex: \(hex)")
            }
            bytes.append(value)
        }
        self.init(bytes)
    }

    var hex: String {
        let digits = Array("0123456789abcdef".unicodeScalars)
        var text = String.UnicodeScalarView()
        for byte in self {
            text.append(digits[Int(byte >> 4)])
            text.append(digits[Int(byte & 0x0F)])
        }
        return String(text)
    }
}
