import Foundation
import XCTest
@testable import BridgeKit

final class MessageTests: XCTestCase {
    /// Decoding the vector's JSON and re-encoding it canonically must reproduce the exact bytes
    /// the host implementation produces.
    func testJSONVectorsRoundTripCanonically() throws {
        let vectors = try PacketVectors.load()
        try assertRoundTrip(HelloMessage.self, vectors.vector(named: "hello-host"))
        try assertRoundTrip(HelloMessage.self, vectors.vector(named: "hello-device"))
        try assertRoundTrip(StartVideoMessage.self, vectors.vector(named: "start-video"))
        try assertRoundTrip(StartAudioMessage.self, vectors.vector(named: "start-audio"))
        try assertRoundTrip(VideoConfigMessage.self, vectors.vector(named: "video-config"))
        try assertRoundTrip(AudioConfigMessage.self, vectors.vector(named: "audio-config"))
        try assertRoundTrip(StatusMessage.self, vectors.vector(named: "status"))
        try assertRoundTrip(LogMessage.self, vectors.vector(named: "log"))
        try assertRoundTrip(ErrorMessage.self, vectors.vector(named: "error"))
    }

    func testDeviceHelloBuiltInCodeMatchesVector() throws {
        let vector = try PacketVectors.load().vector(named: "hello-device")
        let hello = HelloMessage(
            role: .device,
            app: AppDescriptor(name: "mobile-webcam-bridge-ios", version: "0.1.0", build: "42", gitSha: "0123abc"),
            device: DeviceDescriptor(model: "iPhone16,1", name: "iPhone", os: "iOS 26.0"),
            features: ["video.h264", "status", "log", "audio.pcm"]
        )
        XCTAssertEqual(try CanonicalJSON.string(hello), vector.payload.text)
    }

    func testCanonicalJSONEscaping() throws {
        let message = LogMessage(level: .warn, category: "a/b", message: "quote\" backslash\\ tab\t nl\n \u{01} é")
        XCTAssertEqual(
            try CanonicalJSON.string(message),
            #"{"category":"a/b","level":"warn","message":"quote\" backslash\\ tab\t nl\n \u0001 é"}"#
        )
    }

    func testCanonicalJSONRejectsFractionalNumbers() {
        struct Fractional: Encodable { let value: Double }
        XCTAssertThrowsError(try CanonicalJSON.string(Fractional(value: 0.5)))
        XCTAssertEqual(try CanonicalJSON.string(Fractional(value: 2)), #"{"value":2}"#)
    }

    func testCanonicalJSONSortsNestedKeysAndOmitsNil() throws {
        let status = VideoStreamStatus(state: .interrupted, reason: "background")
        XCTAssertEqual(try CanonicalJSON.string(status), #"{"reason":"background","state":"interrupted"}"#)
    }

    func testStartVideoValidation() throws {
        var request = StartVideoMessage(width: 1280, height: 720, fps: 30, bitrateKbps: 6_000, camera: .backWide,
                                        mirror: false, orientation: .auto, encoder: .lowLatency)
        XCTAssertNoThrow(try request.validate())
        request.fps = 120
        XCTAssertThrowsError(try request.validate())
    }

    func testStartVideoBitrateUpperBound() throws {
        var request = StartVideoMessage(width: 3840, height: 2160, fps: 30, bitrateKbps: 40_000, camera: .backWide,
                                        mirror: false, orientation: .auto, encoder: .lowLatency)
        XCTAssertNoThrow(try request.validate())
        request.bitrateKbps = 40_001
        XCTAssertThrowsError(try request.validate())
    }

    func testHostCommandDecoding() throws {
        let vectors = try PacketVectors.load()
        let start = try PacketStreamParser().parseAll(Data(hex: vectors.vector(named: "start-video").hex))[0]
        guard case .startVideo(let request) = try HostCommandDecoder.decode(start) else {
            return XCTFail("expected startVideo")
        }
        XCTAssertEqual(request.camera, .backWide)

        let ping = try PacketStreamParser().parseAll(Data(hex: vectors.vector(named: "ping").hex))[0]
        XCTAssertEqual(try HostCommandDecoder.decode(ping), .ping(t0: 1_000_000))

        let status = try PacketStreamParser().parseAll(Data(hex: vectors.vector(named: "status").hex))[0]
        XCTAssertEqual(try HostCommandDecoder.decode(status), .ignored(rawType: PacketType.status.rawValue))
    }

    func testMalformedJSONIsReportedPerType() {
        let packet = Packet(type: .startAudio, seq: 0, timestampUs: 0, payload: Data("{\"processing\":\"loud\"}".utf8))
        XCTAssertThrowsError(try HostCommandDecoder.decode(packet)) { error in
            XCTAssertEqual((error as? MessageDecodingError)?.type, .startAudio)
        }
    }

    func testLogTruncationKeepsCharacterBoundaries() {
        let message = LogMessage(level: .info, category: "c", message: String(repeating: "é", count: 10))
        let truncated = message.truncated(maxMessageBytes: 5)
        XCTAssertEqual(truncated.message, "éé…")
    }

    private func assertRoundTrip<T: Codable>(_: T.Type, _ vector: PacketVectors.Valid,
                                             file: StaticString = #filePath, line: UInt = #line) throws {
        let text = try XCTUnwrap(vector.payload.text, file: file, line: line)
        let decoded = try JSONDecoder().decode(T.self, from: Data(text.utf8))
        XCTAssertEqual(try CanonicalJSON.string(decoded), text, vector.name, file: file, line: line)
    }
}
