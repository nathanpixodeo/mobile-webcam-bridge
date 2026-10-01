import BridgeKit
import Foundation
import Network

/// Runs the device side of the wire protocol.
///
/// Everything happens on `queue`, the transport queue Network also calls back on: connection
/// events, packet parsing, the session reducer, the send scheduler and the timers. Media and
/// system components hand Sendable values over with `queue.async`, which keeps their FIFO order
/// without creating a Task per frame.
final class SessionCoordinator: ConnectionDriverDelegate, MediaEventSink, @unchecked Sendable {
    private static let tickInterval: DispatchTimeInterval = .milliseconds(100)
    private static let statusIntervalUs: Int64 = 5_000_000
    private static let snapshotIntervalUs: Int64 = 500_000
    /// 10 per 100 ms tick: at most 100 forwarded log entries per second.
    private static let logsPerTick = 10

    private let queue: DispatchQueue
    private let identity: AppIdentity
    private let clock: any MonotonicClock
    private let logger: AppLogger
    private let diagnostics: any DiagnosticsSource
    private let bootReport: [String]
    private let onSnapshot: @Sendable (SessionSnapshot) -> Void
    private var media: (any MediaControlling)?

    // Transport-queue state.
    private var state = SessionState()
    private var pendingEvents: [SessionEvent] = []
    private var isApplying = false
    private var drivers: [ConnectionID: ConnectionDriver] = [:]
    private var activeConnection: ConnectionID?
    private var nextConnectionNumber: UInt64 = 1
    private var parser = PacketStreamParser()
    private var scheduler = SendScheduler()
    private var factory = OutboundPacketFactory()
    private var watchdog = HeartbeatWatchdog()
    private var clockOffset = ClockOffsetEstimator()
    private var currentVideoConfigId: Int?
    private var currentAudioConfigId: Int?
    private var videoStatus = VideoStreamStatus.off
    private var audioStatus = AudioStreamStatus.off
    private var system = SystemSnapshot()
    private var serverState = CompanionServer.State.starting
    private var lastSentStatus: StatusMessage?
    private var lastStatusSentUs: Int64 = 0
    private var traffic = TrafficStats()
    private var lastSnapshot: SessionSnapshot?
    private var lastSnapshotUs: Int64 = 0
    private var timer: (any DispatchSourceTimer)?

    init(queue: DispatchQueue, identity: AppIdentity, clock: any MonotonicClock, logger: AppLogger,
         diagnostics: any DiagnosticsSource, bootReport: [String],
         onSnapshot: @escaping @Sendable (SessionSnapshot) -> Void) {
        self.queue = queue
        self.identity = identity
        self.clock = clock
        self.logger = logger
        self.diagnostics = diagnostics
        self.bootReport = bootReport
        self.onSnapshot = onSnapshot
    }

    /// Breaks the construction cycle: the media pipelines report to this object.
    func bind(media: any MediaControlling) {
        queue.async { [self] in self.media = media }
    }

    func start() {
        queue.async { [self] in
            guard timer == nil else { return }
            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now() + Self.tickInterval, repeating: Self.tickInterval, leeway: .milliseconds(10))
            timer.setEventHandler { [weak self] in self?.tick() }
            timer.resume()
            self.timer = timer
            publishSnapshot(force: true)
        }
    }

    // MARK: - Inputs from the server and the system monitor

    func serverStateChanged(_ newState: CompanionServer.State) {
        queue.async { [self] in
            serverState = newState
            publishSnapshot(force: true)
        }
    }

    /// Called by the listener, which runs on the same transport queue.
    func accept(_ connection: NWConnection) {
        dispatchPrecondition(condition: .onQueue(queue))
        let id = ConnectionID(nextConnectionNumber)
        nextConnectionNumber += 1
        let driver = ConnectionDriver(id: id, connection: connection, queue: queue, delegate: self)
        drivers[id] = driver
        logger.info("transport", "Host connecting from \(driver.remoteDescription) (\(id))")
        driver.start()
    }

    func updateSystem(_ snapshot: SystemSnapshot) {
        queue.async { [self] in
            guard snapshot != system else { return }
            system = snapshot
            sendStatus(force: false)
        }
    }

    // MARK: - ConnectionDriverDelegate (transport queue)

    func connectionDidOpen(_ id: ConnectionID) {
        logger.info("transport", "Connection \(id) open")
        activeConnection = id
        resetTransportState()
        apply(.connectionOpened(id))
        publishSnapshot(force: true)
    }

    func connection(_ id: ConnectionID, didReceive data: Data) {
        guard id == activeConnection else { return }
        let receivedAt = clock.nowMicroseconds()
        let packets: [Packet]
        do {
            packets = try parser.push(data)
        } catch let error as ProtocolError {
            logger.error("session", "Protocol violation: \(error.code)")
            apply(.protocolViolation(id, error))
            return
        } catch {
            logger.error("session", "Unexpected parser error: \(error)")
            return
        }

        for packet in packets {
            guard id == activeConnection else { return }
            watchdog.noteInbound(nowUs: receivedAt)
            let command: HostCommand
            do {
                command = try HostCommandDecoder.decode(packet)
            } catch let error as MessageDecodingError {
                logger.warn("session", "Rejected \(error.type): \(error.detail)")
                apply(.commandRejected(id, error))
                continue
            } catch let error as ProtocolError {
                apply(.protocolViolation(id, error))
                return
            } catch {
                logger.error("session", "Unexpected decode error: \(error)")
                continue
            }
            apply(.commandReceived(id, command, receivedAtUs: receivedAt))
        }
    }

    func connection(_ id: ConnectionID, didFinishSending byteCount: Int) {
        guard id == activeConnection else { return }
        scheduler.acknowledge(bytes: byteCount)
        pump()
    }

    func connectionDidClose(_ id: ConnectionID, reason: String) {
        drivers[id] = nil
        logger.info("transport", "Connection \(id) closed: \(reason)")
        guard id == activeConnection else { return }
        activeConnection = nil
        resetTransportState()
        apply(.connectionClosed(id))
        publishSnapshot(force: true)
    }

    // MARK: - MediaEventSink (any thread)

    func videoConfigured(_ config: VideoConfigMessage) {
        queue.async { [self] in
            guard state.readyConnection != nil, state.video != nil else { return }
            currentVideoConfigId = config.configId
            // Older frames belong to the previous configuration.
            scheduler.purgeVideo()
            logger.info("session", "VideoConfig \(config.configId): \(config.width)x\(config.height)@\(config.fps) "
                + "\(config.bitrateKbps) kbps \(config.encoder.rawValue)")
            sendJSON(.videoConfig, config, kind: .control)
        }
    }

    func videoEncoded(_ frame: EncodedVideoFrame) {
        queue.async { [self] in
            guard state.readyConnection != nil, frame.configId == currentVideoConfigId else { return }
            do {
                let packet = try factory.videoAccessUnit(frame)
                traffic.record(videoBytes: frame.annexB.count)
                enqueue(packet, kind: .video(isKeyframe: frame.isKeyframe, isDisposable: frame.isDisposable))
            } catch {
                logger.warn("session", "Dropped an oversized access unit (\(frame.annexB.count) bytes)")
                media?.requestKeyframe()
            }
        }
    }

    func videoStatusChanged(_ status: VideoStreamStatus) {
        queue.async { [self] in
            videoStatus = status
            sendStatus(force: false)
            publishSnapshot(force: true)
        }
    }

    func videoFailed(_ error: ErrorMessage) {
        queue.async { [self] in apply(.videoFailed(error)) }
    }

    func audioConfigured(_ config: AudioConfigMessage) {
        queue.async { [self] in
            guard state.readyConnection != nil, state.audio != nil else { return }
            currentAudioConfigId = config.configId
            sendJSON(.audioConfig, config, kind: .control)
        }
    }

    func audioCaptured(_ chunk: AudioChunk) {
        queue.async { [self] in
            guard state.readyConnection != nil, chunk.configId == currentAudioConfigId else { return }
            do {
                let packet = try factory.audioChunk(chunk)
                enqueue(packet, kind: .audio)
            } catch {
                logger.warn("session", "Dropped an oversized audio chunk (\(chunk.pcm.count) bytes)")
            }
        }
    }

    func audioStatusChanged(_ status: AudioStreamStatus) {
        queue.async { [self] in
            audioStatus = status
            sendStatus(force: false)
            publishSnapshot(force: true)
        }
    }

    func audioFailed(_ error: ErrorMessage) {
        queue.async { [self] in apply(.audioFailed(error)) }
    }

    // MARK: - Reducer

    /// Run-to-completion: events raised while effects execute are queued, never nested.
    private func apply(_ event: SessionEvent) {
        pendingEvents.append(event)
        guard !isApplying else { return }
        isApplying = true
        defer { isApplying = false }
        while !pendingEvents.isEmpty {
            let next = pendingEvents.removeFirst()
            let transition = SessionReducer.reduce(state, next)
            state = transition.state
            transition.effects.forEach(execute)
        }
    }

    private func execute(_ effect: SessionEffect) {
        switch effect {
        case .sendHello(let id):
            guard id == activeConnection else { return }
            sendJSON(.hello, identity.hello, kind: .control)

        case .sendError(let id, let message):
            guard id == activeConnection else { return }
            logger.warn("session", "Sending error \(message.code.rawValue): \(message.message)")
            sendJSON(.error, message, kind: .control)

        case .sendPong(let id, let payload):
            guard id == activeConnection else { return }
            enqueue(factory.pong(payload, timestampUs: clock.nowMicroseconds()), kind: .control)

        case .recordClockSample(let t0, let t1, let t2, let t3):
            clockOffset.add(t0: t0, t1: t1, t2: t2, t3: t3)

        case .handshakeCompleted(let id, let minor):
            guard id == activeConnection else { return }
            logger.info("session", "Host handshake complete (protocol 1.\(minor))")
            watchdog.start(nowUs: clock.nowMicroseconds())
            sendBootReport()
            sendStatus(force: true)

        case .closeConnection(let id, let reason):
            logger.info("session", "Closing connection \(id): \(reason)")
            if id == activeConnection {
                activeConnection = nil
                resetTransportState()
            }
            // Deferred so the driver's close callback never re-enters the reducer. Network still
            // delivers data handed over before the cancel (e.g. a fatal Error packet).
            if let driver = drivers[id] {
                queue.async { driver.close(reason: "\(reason)") }
            }

        case .startVideo(let request):
            currentVideoConfigId = nil
            scheduler.purgeVideo()
            logger.info("session", "Host requested video \(request.width)x\(request.height)@\(request.fps) "
                + "\(request.camera.rawValue), \(request.encoder.rawValue)")
            media?.startVideo(request)

        case .stopVideo:
            currentVideoConfigId = nil
            scheduler.purgeVideo()
            logger.info("session", "Video stopped")
            media?.stopVideo()

        case .requestKeyframe:
            media?.requestKeyframe()

        case .startAudio(let request):
            currentAudioConfigId = nil
            logger.info("session", "Host requested audio (\(request.processing.rawValue))")
            media?.startAudio(request)

        case .stopAudio:
            currentAudioConfigId = nil
            logger.info("session", "Audio stopped")
            media?.stopAudio()
        }
    }

    // MARK: - Sending

    private func sendJSON<Message: Encodable>(_ type: PacketType, _ message: Message, kind: OutboundClass) {
        do {
            let packet = try factory.json(type, message, timestampUs: clock.nowMicroseconds())
            enqueue(packet, kind: kind)
        } catch {
            logger.error("session", "Cannot encode \(type): \(error)")
        }
    }

    private func enqueue(_ packet: Packet, kind: OutboundClass) {
        guard let id = activeConnection else { return }
        let result = scheduler.enqueue(OutboundItem(packet: packet, kind: kind))
        traffic.droppedVideoFrames += result.droppedVideoFrames
        if result.keyframeNeeded { media?.requestKeyframe() }
        if result.audioBacklogExceeded {
            logger.error("session", "More than one second of audio is queued; closing the connection")
            apply(.audioBacklogExceeded(id))
            return
        }
        pump()
    }

    private func pump() {
        guard let id = activeConnection, let driver = drivers[id] else { return }
        while let item = scheduler.dequeue() {
            driver.send(item.bytes)
        }
    }

    private func sendLog(_ entry: LogEntry) {
        // Never log from here: a failure would feed itself through the forwarding ring.
        guard let packet = try? factory.json(.log, entry.logMessage.truncated(), timestampUs: entry.timestampUs) else { return }
        enqueue(packet, kind: .log)
    }

    private func forwardLogs() {
        let batch = logger.takeForwardingBatch(maxCount: Self.logsPerTick)
        if batch.dropped > 0 {
            sendLog(LogEntry(timestampUs: clock.nowMicroseconds(), level: .warn, category: "log",
                             message: "\(batch.dropped) log entries were dropped before forwarding"))
        }
        batch.entries.forEach(sendLog)
    }

    private func sendBootReport() {
        let now = clock.nowMicroseconds()
        let app = identity.app
        let device = identity.device
        var lines = ["\(device.model), \(device.os), app \(app.version) (\(app.build)), git \(app.gitSha)"]
        lines += bootReport
        for line in lines {
            sendLog(LogEntry(timestampUs: now, level: .info, category: "boot", message: line))
        }
        for report in diagnostics.takePendingReports(limit: 5) {
            sendLog(LogEntry(timestampUs: now, level: .warn, category: "diagnostics", message: report))
        }
    }

    private func sendStatus(force: Bool) {
        guard state.readyConnection != nil else { return }
        let status = StatusMessage(appState: system.appState, audio: audioStatus, battery: system.battery,
                                   lowPower: system.lowPower, permissions: system.permissions,
                                   thermal: system.thermal.statusName, video: videoStatus)
        guard force || status != lastSentStatus else { return }
        lastSentStatus = status
        lastStatusSentUs = clock.nowMicroseconds()
        sendJSON(.status, status, kind: .control)
    }

    // MARK: - Timer

    private func tick() {
        let now = clock.nowMicroseconds()
        if let id = state.readyConnection {
            switch watchdog.tick(nowUs: now) {
            case .sendPing?:
                enqueue(factory.ping(timestampUs: now), kind: .control)
            case .timedOut?:
                logger.warn("session", "No packet from the host for 4 s")
                apply(.heartbeatTimedOut(id))
            case nil:
                break
            }
        }
        if state.readyConnection != nil {
            forwardLogs()
            if now - lastStatusSentUs >= Self.statusIntervalUs { sendStatus(force: true) }
        }
        traffic.roll(nowUs: now)
        publishSnapshot(force: false)
    }

    // MARK: - State helpers

    private func resetTransportState() {
        parser = PacketStreamParser()
        scheduler.reset()
        factory = OutboundPacketFactory()
        watchdog.stop()
        clockOffset.reset()
        currentVideoConfigId = nil
        currentAudioConfigId = nil
        lastSentStatus = nil
    }

    private var connectionStatus: ConnectionStatus {
        switch state.phase {
        case .ready:
            return .connected
        case .awaitingHello:
            return .handshaking
        case .idle:
            switch serverState {
            case .listening(let port): return .listening(port: port)
            case .failed(let message): return .listenerFailed(message)
            case .starting, .stopped: return .starting
            }
        }
    }

    private func publishSnapshot(force: Bool) {
        let now = clock.nowMicroseconds()
        guard force || now - lastSnapshotUs >= Self.snapshotIntervalUs else { return }
        lastSnapshotUs = now
        let snapshot = SessionSnapshot(
            connection: connectionStatus,
            video: videoStatus,
            audio: audioStatus,
            roundTripMs: clockOffset.best.map { Int($0.roundTripUs / 1_000) },
            videoFps: traffic.framesPerSecond,
            videoKbps: traffic.kilobitsPerSecond,
            droppedVideoFrames: traffic.droppedVideoFrames
        )
        guard snapshot != lastSnapshot else { return }
        lastSnapshot = snapshot
        onSnapshot(snapshot)
    }
}

/// Outgoing video rate over the last second, for the diagnostics screen.
private struct TrafficStats {
    private var frames = 0
    private var bytes = 0
    private var windowStartUs: Int64 = 0
    private(set) var framesPerSecond = 0
    private(set) var kilobitsPerSecond = 0
    var droppedVideoFrames = 0

    mutating func record(videoBytes: Int) {
        frames += 1
        bytes += videoBytes
    }

    mutating func roll(nowUs: Int64) {
        let elapsed = nowUs - windowStartUs
        guard elapsed >= 1_000_000 else { return }
        framesPerSecond = Int(Int64(frames) * 1_000_000 / elapsed)
        kilobitsPerSecond = Int(Int64(bytes) * 8_000 / elapsed)
        frames = 0
        bytes = 0
        windowStartUs = nowUs
    }
}
