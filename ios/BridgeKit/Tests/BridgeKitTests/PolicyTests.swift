import XCTest
@testable import BridgeKit

final class ThermalPolicyTests: XCTestCase {
    private let policy = ThermalPolicy()

    func testThrottleTable() {
        XCTAssertEqual(policy.throttle(for: .nominal), .unrestricted)
        XCTAssertEqual(policy.throttle(for: .fair), .unrestricted)
        XCTAssertEqual(policy.throttle(for: .serious), VideoThrottle(maxFps: 24, bitratePermille: 700, videoAllowed: true))
        XCTAssertEqual(policy.throttle(for: .critical), VideoThrottle(maxFps: 15, bitratePermille: 500, videoAllowed: true))
        XCTAssertFalse(policy.throttle(for: .shutdown).videoAllowed)
    }

    func testApply() {
        let serious = policy.apply(policy.throttle(for: .serious), fps: 30, bitrateKbps: 6_000)
        XCTAssertEqual(serious.fps, 24)
        XCTAssertEqual(serious.bitrateKbps, 4_200)

        let lowFps = policy.apply(policy.throttle(for: .serious), fps: 15, bitrateKbps: 1_000)
        XCTAssertEqual(lowFps.fps, 15, "a cap never raises the frame rate")
    }

    func testStatusNameFoldsShutdownIntoCritical() {
        XCTAssertEqual(ThermalLevel.shutdown.statusName, .critical)
        XCTAssertTrue(ThermalLevel.serious > ThermalLevel.fair)
    }
}

final class FormatSelectorTests: XCTestCase {
    private let selector = FormatSelector()

    private let formats = [
        CaptureFormatInfo(index: 0, width: 640, height: 480, minFrameRate: 1, maxFrameRate: 30, isVideoRange420: true),
        CaptureFormatInfo(index: 1, width: 1280, height: 720, minFrameRate: 1, maxFrameRate: 60, isVideoRange420: true),
        CaptureFormatInfo(index: 2, width: 1280, height: 720, minFrameRate: 1, maxFrameRate: 30, isVideoRange420: true),
        CaptureFormatInfo(index: 3, width: 1280, height: 720, minFrameRate: 1, maxFrameRate: 30, isVideoRange420: false),
        CaptureFormatInfo(index: 4, width: 1920, height: 1080, minFrameRate: 1, maxFrameRate: 60, isVideoRange420: true),
        CaptureFormatInfo(index: 5, width: 4032, height: 3024, minFrameRate: 1, maxFrameRate: 30, isVideoRange420: true),
    ]

    func testExactMatchPrefersLowestSufficientFrameRate() {
        XCTAssertEqual(selector.select(from: formats, width: 1280, height: 720, fps: 30)?.index, 2)
    }

    func testFrameRateRequirementIsHonoured() {
        XCTAssertEqual(selector.select(from: formats, width: 1280, height: 720, fps: 60)?.index, 1)
    }

    func testSmallestLargerSameAspect() {
        XCTAssertEqual(selector.select(from: formats, width: 960, height: 540, fps: 30)?.index, 2)
    }

    func testFallsBackToLargestWhenNothingCovers() {
        XCTAssertEqual(selector.select(from: formats, width: 7680, height: 4320, fps: 30)?.index, 5)
    }

    func testEmptyInput() {
        XCTAssertNil(selector.select(from: [], width: 1280, height: 720, fps: 30))
    }
}

final class LogRingTests: XCTestCase {
    func testCapacityDropsOldest() {
        var ring = LogRing(capacity: 2)
        for index in 0..<3 {
            ring.append(LogEntry(timestampUs: Int64(index), level: .info, category: "c", message: "\(index)"))
        }
        XCTAssertEqual(ring.count, 2)
        XCTAssertEqual(ring.takeDroppedCount(), 1)
        XCTAssertEqual(ring.takeDroppedCount(), 0)
        XCTAssertEqual(ring.newest(5).map(\.message), ["1", "2"])
        XCTAssertEqual(ring.drain(maxCount: 1).map(\.message), ["1"])
        XCTAssertEqual(ring.count, 1)
    }
}
