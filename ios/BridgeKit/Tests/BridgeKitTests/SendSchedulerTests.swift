import Foundation
import XCTest
@testable import BridgeKit

final class SendSchedulerTests: XCTestCase {
    private var seq: UInt32 = 0

    func testPriorityOrder() {
        var scheduler = SendScheduler()
        _ = scheduler.enqueue(item(.log))
        _ = scheduler.enqueue(item(.video(isKeyframe: true, isDisposable: false)))
        _ = scheduler.enqueue(item(.audio))
        _ = scheduler.enqueue(item(.control))

        var order: [OutboundClass] = []
        while let next = scheduler.dequeue() {
            order.append(next.kind)
            scheduler.acknowledge(bytes: next.bytes.count)
        }
        XCTAssertEqual(order, [.control, .audio, .video(isKeyframe: true, isDisposable: false), .log])
    }

    func testWindowLimitsInFlightBytes() {
        var scheduler = SendScheduler(configuration: .init(windowBytes: 100))
        _ = scheduler.enqueue(item(.audio, payloadSize: 60))
        _ = scheduler.enqueue(item(.audio, payloadSize: 60))

        let first = scheduler.dequeue()
        XCTAssertNotNil(first)
        XCTAssertNil(scheduler.dequeue(), "second item would exceed the window")
        scheduler.acknowledge(bytes: first?.bytes.count ?? 0)
        XCTAssertNotNil(scheduler.dequeue())
    }

    func testOversizedItemIsSentWhenNothingIsInFlight() {
        var scheduler = SendScheduler(configuration: .init(windowBytes: 10))
        _ = scheduler.enqueue(item(.video(isKeyframe: true, isDisposable: false), payloadSize: 500))
        XCTAssertNotNil(scheduler.dequeue())
    }

    func testDisposableFramesAreDroppedFirst() {
        var scheduler = SendScheduler(configuration: .init(maxQueuedVideoFrames: 3))
        _ = scheduler.enqueue(item(.video(isKeyframe: true, isDisposable: false)))
        _ = scheduler.enqueue(item(.video(isKeyframe: false, isDisposable: true)))
        _ = scheduler.enqueue(item(.video(isKeyframe: false, isDisposable: false)))
        let result = scheduler.enqueue(item(.video(isKeyframe: false, isDisposable: false)))

        XCTAssertEqual(result.droppedVideoFrames, 1)
        XCTAssertFalse(result.keyframeNeeded)
        XCTAssertEqual(scheduler.queuedVideoFrames, 3)
    }

    func testReferenceDropRequestsKeyframeAndDropsUntilIDR() {
        var scheduler = SendScheduler(configuration: .init(maxQueuedVideoFrames: 2))
        _ = scheduler.enqueue(item(.video(isKeyframe: true, isDisposable: false)))
        _ = scheduler.enqueue(item(.video(isKeyframe: false, isDisposable: false)))
        let overflow = scheduler.enqueue(item(.video(isKeyframe: false, isDisposable: false)))

        XCTAssertTrue(overflow.keyframeNeeded)
        XCTAssertEqual(overflow.droppedVideoFrames, 3)
        XCTAssertEqual(scheduler.queuedVideoFrames, 0)

        let pFrame = scheduler.enqueue(item(.video(isKeyframe: false, isDisposable: false)))
        XCTAssertEqual(pFrame.droppedVideoFrames, 1, "P frames are useless until the next IDR")
        XCTAssertEqual(scheduler.queuedVideoFrames, 0)

        let idr = scheduler.enqueue(item(.video(isKeyframe: true, isDisposable: false)))
        XCTAssertEqual(idr.droppedVideoFrames, 0)
        XCTAssertEqual(scheduler.queuedVideoFrames, 1)
    }

    func testNewKeyframeSupersedesQueuedFrames() {
        var scheduler = SendScheduler(configuration: .init(maxQueuedVideoFrames: 2))
        _ = scheduler.enqueue(item(.video(isKeyframe: true, isDisposable: false)))
        _ = scheduler.enqueue(item(.video(isKeyframe: false, isDisposable: false)))
        let result = scheduler.enqueue(item(.video(isKeyframe: true, isDisposable: false)))

        XCTAssertFalse(result.keyframeNeeded)
        XCTAssertEqual(result.droppedVideoFrames, 2)
        XCTAssertEqual(scheduler.queuedVideoFrames, 1)
        XCTAssertEqual(scheduler.dequeue()?.kind, .video(isKeyframe: true, isDisposable: false))
    }

    func testAudioIsNeverDroppedButBacklogIsReported() {
        var scheduler = SendScheduler(configuration: .init(maxQueuedAudioBytes: 1_000))
        var backlog = false
        for _ in 0..<20 {
            let result = scheduler.enqueue(item(.audio, payloadSize: 100))
            XCTAssertEqual(result.droppedVideoFrames, 0)
            backlog = backlog || result.audioBacklogExceeded
        }
        XCTAssertTrue(backlog)
        var count = 0
        while let next = scheduler.dequeue() {
            count += 1
            scheduler.acknowledge(bytes: next.bytes.count)
        }
        XCTAssertEqual(count, 20)
        XCTAssertEqual(scheduler.queuedAudioBytes, 0)
    }

    func testLogsAreBounded() {
        var scheduler = SendScheduler(configuration: .init(maxQueuedLogs: 2))
        _ = scheduler.enqueue(item(.log))
        _ = scheduler.enqueue(item(.log))
        let result = scheduler.enqueue(item(.log))
        XCTAssertEqual(result.droppedLogs, 1)
        XCTAssertEqual(scheduler.queuedLogCount, 2)
    }

    func testPurgeVideoClearsDropMode() {
        var scheduler = SendScheduler(configuration: .init(maxQueuedVideoFrames: 1))
        _ = scheduler.enqueue(item(.video(isKeyframe: true, isDisposable: false)))
        _ = scheduler.enqueue(item(.video(isKeyframe: false, isDisposable: false)))
        scheduler.purgeVideo()
        let result = scheduler.enqueue(item(.video(isKeyframe: false, isDisposable: false)))
        XCTAssertEqual(result.droppedVideoFrames, 0)
    }

    private func item(_ kind: OutboundClass, payloadSize: Int = 10) -> OutboundItem {
        seq += 1
        let type: PacketType = switch kind {
        case .control: .ping
        case .audio: .audioChunk
        case .video: .videoAccessUnit
        case .log: .log
        }
        let payload = type == .ping ? Data() : Data(repeating: 0xAB, count: payloadSize)
        return OutboundItem(packet: Packet(type: type, seq: seq, timestampUs: 0, payload: payload), kind: kind)
    }
}
