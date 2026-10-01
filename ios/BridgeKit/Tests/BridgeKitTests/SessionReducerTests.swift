import Foundation
import XCTest
@testable import BridgeKit

final class SessionReducerTests: XCTestCase {
    private let first = ConnectionID(1)
    private let second = ConnectionID(2)
    private let video = StartVideoMessage(width: 1280, height: 720, fps: 30, bitrateKbps: 6_000, camera: .backWide,
                                          mirror: false, orientation: .auto, encoder: .lowLatency)
    private let audio = StartAudioMessage(processing: .standard)

    func testConnectionOpenSendsHello() {
        let transition = SessionReducer.reduce(SessionState(), .connectionOpened(first))
        XCTAssertEqual(transition.state.phase, .awaitingHello(first))
        XCTAssertEqual(transition.effects, [.sendHello(first)])
    }

    func testCommandsBeforeHelloAreIgnored() {
        var state = reduce(SessionState(), .connectionOpened(first)).state
        let transition = reduce(state, .commandReceived(first, .startVideo(video), receivedAtUs: 0))
        state = transition.state
        XCTAssertEqual(transition.effects, [])
        XCTAssertNil(state.video)
    }

    func testHandshakeCompletesWithEffectiveMinor() {
        let state = reduce(SessionState(), .connectionOpened(first)).state
        let transition = reduce(state, .commandReceived(first, .hello(hostHello(major: 1, minor: 3)), receivedAtUs: 0))
        XCTAssertEqual(transition.state.phase, .ready(first, effectiveMinor: 0))
        XCTAssertEqual(transition.effects, [.handshakeCompleted(first, effectiveMinor: 0)])
    }

    func testVersionMismatchSendsFatalErrorAndCloses() {
        let state = reduce(SessionState(), .connectionOpened(first)).state
        let transition = reduce(state, .commandReceived(first, .hello(hostHello(major: 2, minor: 0)), receivedAtUs: 0))
        XCTAssertEqual(transition.state.phase, .idle)
        XCTAssertEqual(transition.effects, [
            .sendError(first, ErrorMessage(code: .versionMismatch, message: "Unsupported protocol major version 2", fatal: true)),
            .closeConnection(first, .versionMismatch),
        ])
    }

    func testStartVideoIsIdempotent() {
        var state = readyState()
        let start = reduce(state, .commandReceived(first, .startVideo(video), receivedAtUs: 0))
        XCTAssertEqual(start.effects, [.startVideo(video)])
        state = start.state
        XCTAssertEqual(reduce(state, .commandReceived(first, .startVideo(video), receivedAtUs: 0)).effects, [])

        var changed = video
        changed.fps = 60
        XCTAssertEqual(reduce(state, .commandReceived(first, .startVideo(changed), receivedAtUs: 0)).effects,
                       [.startVideo(changed)])
    }

    func testPingIsAnsweredWithReceiveTime() {
        let transition = reduce(readyState(), .commandReceived(first, .ping(t0: 100), receivedAtUs: 250))
        XCTAssertEqual(transition.effects, [.sendPong(first, PongPayload(echoT0: 100, t1: 250))])
    }

    func testPongProducesClockSample() {
        let pong = HostCommand.pong(PongPayload(echoT0: 10, t1: 50), sentAtUs: 60)
        let transition = reduce(readyState(), .commandReceived(first, pong, receivedAtUs: 100))
        XCTAssertEqual(transition.effects, [.recordClockSample(t0: 10, t1: 50, t2: 60, t3: 100)])
    }

    func testDisconnectStopsRunningStreams() {
        var state = readyState()
        state = reduce(state, .commandReceived(first, .startVideo(video), receivedAtUs: 0)).state
        state = reduce(state, .commandReceived(first, .startAudio(audio), receivedAtUs: 0)).state

        let transition = reduce(state, .connectionClosed(first))
        XCTAssertEqual(transition.state.phase, .idle)
        XCTAssertEqual(transition.effects, [.stopVideo, .stopAudio])
    }

    func testNewConnectionReplacesOld() {
        var state = readyState()
        state = reduce(state, .commandReceived(first, .startVideo(video), receivedAtUs: 0)).state

        let transition = reduce(state, .connectionOpened(second))
        XCTAssertEqual(transition.state.phase, .awaitingHello(second))
        XCTAssertEqual(transition.effects, [.closeConnection(first, .replaced), .stopVideo, .sendHello(second)])

        let late = reduce(transition.state, .connectionClosed(first))
        XCTAssertEqual(late.effects, [], "closing the replaced connection must not affect the new one")
        XCTAssertEqual(late.state.phase, .awaitingHello(second))
    }

    func testCommandsFromStaleConnectionAreIgnored() {
        let state = reduce(readyState(), .connectionOpened(second)).state
        XCTAssertEqual(reduce(state, .commandReceived(first, .startVideo(video), receivedAtUs: 0)).effects, [])
    }

    func testHeartbeatTimeoutTearsDown() {
        var state = readyState()
        state = reduce(state, .commandReceived(first, .startAudio(audio), receivedAtUs: 0)).state
        let transition = reduce(state, .heartbeatTimedOut(first))
        XCTAssertEqual(transition.effects, [.stopAudio, .closeConnection(first, .heartbeatTimeout)])
    }

    func testRejectedCommandWhileReadyIsNonFatal() {
        let error = MessageDecodingError.invalidJSON(type: .startVideo, detail: "x")
        let transition = reduce(readyState(), .commandRejected(first, error))
        XCTAssertEqual(transition.effects, [
            .sendError(first, ErrorMessage(code: .badRequest, message: "Invalid startVideo: x", fatal: false)),
        ])
        XCTAssertEqual(transition.state.phase, .ready(first, effectiveMinor: 0))
    }

    func testVideoFailureClearsDesiredVideoSoARetryRestarts() {
        var state = readyState()
        state = reduce(state, .commandReceived(first, .startVideo(video), receivedAtUs: 0)).state
        let failure = ErrorMessage(code: .cameraUnavailable, message: "busy", fatal: false)
        let failed = reduce(state, .videoFailed(failure))
        XCTAssertEqual(failed.effects, [.sendError(first, failure)])
        XCTAssertNil(failed.state.video)
        XCTAssertEqual(reduce(failed.state, .commandReceived(first, .startVideo(video), receivedAtUs: 0)).effects,
                       [.startVideo(video)])
    }

    func testRequestKeyframeOnlyWhileVideoRuns() {
        XCTAssertEqual(reduce(readyState(), .commandReceived(first, .requestKeyframe, receivedAtUs: 0)).effects, [])
        let state = reduce(readyState(), .commandReceived(first, .startVideo(video), receivedAtUs: 0)).state
        XCTAssertEqual(reduce(state, .commandReceived(first, .requestKeyframe, receivedAtUs: 0)).effects, [.requestKeyframe])
    }

    // MARK: - Helpers

    private func reduce(_ state: SessionState, _ event: SessionEvent) -> SessionTransition {
        SessionReducer.reduce(state, event)
    }

    private func readyState() -> SessionState {
        let opened = reduce(SessionState(), .connectionOpened(first)).state
        return reduce(opened, .commandReceived(first, .hello(hostHello(major: 1, minor: 0)), receivedAtUs: 0)).state
    }

    private func hostHello(major: Int, minor: Int) -> HelloMessage {
        HelloMessage(protocolVersion: ProtocolVersion(major: major, minor: minor), role: .host,
                     app: AppDescriptor(name: "host", version: "0", build: "0", gitSha: "0"), features: [])
    }
}
