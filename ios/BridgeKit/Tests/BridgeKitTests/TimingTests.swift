import XCTest
@testable import BridgeKit

final class KeyframeLimiterTests: XCTestCase {
    func testRequestsAreCoalescedAndRateLimited() {
        var limiter = KeyframeLimiter(minIntervalUs: 500_000)
        XCTAssertFalse(limiter.shouldForceKeyframe(nowUs: 0), "nothing pending")

        limiter.request()
        limiter.request()
        XCTAssertTrue(limiter.shouldForceKeyframe(nowUs: 1_000))
        XCTAssertFalse(limiter.shouldForceKeyframe(nowUs: 2_000), "coalesced into one IDR")

        limiter.request()
        XCTAssertFalse(limiter.shouldForceKeyframe(nowUs: 400_000), "too soon")
        XCTAssertTrue(limiter.isPending)
        XCTAssertTrue(limiter.shouldForceKeyframe(nowUs: 501_000))
    }

    func testNaturalKeyframeClearsPendingRequest() {
        var limiter = KeyframeLimiter()
        limiter.request()
        limiter.noteKeyframe(atUs: 10)
        XCTAssertFalse(limiter.isPending)
    }
}

final class HeartbeatWatchdogTests: XCTestCase {
    func testPingsEverySecondAndTimesOutAfterFour() {
        var watchdog = HeartbeatWatchdog()
        XCTAssertNil(watchdog.tick(nowUs: 0), "not started")

        watchdog.start(nowUs: 0)
        XCTAssertEqual(watchdog.tick(nowUs: 100_000), .sendPing)
        XCTAssertNil(watchdog.tick(nowUs: 600_000))
        XCTAssertEqual(watchdog.tick(nowUs: 1_100_000), .sendPing)

        watchdog.noteInbound(nowUs: 2_000_000)
        XCTAssertNotEqual(watchdog.tick(nowUs: 5_900_000), .timedOut)
        XCTAssertEqual(watchdog.tick(nowUs: 6_000_000), .timedOut)
        XCTAssertFalse(watchdog.isRunning)
    }
}

final class ClockOffsetEstimatorTests: XCTestCase {
    func testOffsetAndRoundTrip() throws {
        var estimator = ClockOffsetEstimator()
        // Peer clock is 1 000 µs ahead; 100 µs each way; peer took 50 µs to answer.
        let sample = try XCTUnwrap(estimator.add(t0: 0, t1: 1_100, t2: 1_150, t3: 250))
        XCTAssertEqual(sample.roundTripUs, 200)
        XCTAssertEqual(sample.offsetUs, 1_000)
    }

    func testBestSampleHasLowestRoundTrip() {
        var estimator = ClockOffsetEstimator(windowSize: 3)
        estimator.add(t0: 0, t1: 500, t2: 500, t3: 1_000)      // rtt 1000
        estimator.add(t0: 0, t1: 60, t2: 60, t3: 100)          // rtt 100
        estimator.add(t0: 0, t1: 300, t2: 300, t3: 600)        // rtt 600
        XCTAssertEqual(estimator.best?.roundTripUs, 100)
        estimator.add(t0: 0, t1: 300, t2: 300, t3: 600)
        estimator.add(t0: 0, t1: 300, t2: 300, t3: 600)
        XCTAssertEqual(estimator.best?.roundTripUs, 600, "old samples leave the window")
    }

    func testNegativeRoundTripIsDiscarded() {
        var estimator = ClockOffsetEstimator()
        XCTAssertNil(estimator.add(t0: 100, t1: 0, t2: 1_000, t3: 200))
    }
}
