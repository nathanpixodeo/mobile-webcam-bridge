package dev.mobilewebcambridge.android.session

import dev.mobilewebcambridge.android.core.AppIdentity
import dev.mobilewebcambridge.android.core.AppLogger
import dev.mobilewebcambridge.android.core.SerialThread
import dev.mobilewebcambridge.android.media.MediaControlling
import dev.mobilewebcambridge.android.media.MediaEventSink
import dev.mobilewebcambridge.android.transport.CompanionServer
import dev.mobilewebcambridge.android.transport.ConnectionDriver
import dev.mobilewebcambridge.android.transport.ConnectionListener
import dev.mobilewebcambridge.protocol.logging.LogEntry
import dev.mobilewebcambridge.protocol.media.AudioChunk
import dev.mobilewebcambridge.protocol.media.EncodedVideoFrame
import dev.mobilewebcambridge.protocol.messages.AudioConfigMessage
import dev.mobilewebcambridge.protocol.messages.AudioStreamStatus
import dev.mobilewebcambridge.protocol.messages.ErrorMessage
import dev.mobilewebcambridge.protocol.messages.HostCommand
import dev.mobilewebcambridge.protocol.messages.HostCommandDecoder
import dev.mobilewebcambridge.protocol.messages.JsonMessage
import dev.mobilewebcambridge.protocol.messages.LogLevel
import dev.mobilewebcambridge.protocol.messages.MessageDecodingException
import dev.mobilewebcambridge.protocol.messages.OutboundPacketFactory
import dev.mobilewebcambridge.protocol.messages.StatusMessage
import dev.mobilewebcambridge.protocol.messages.VideoConfigMessage
import dev.mobilewebcambridge.protocol.messages.VideoStreamStatus
import dev.mobilewebcambridge.protocol.session.ClockOffsetEstimator
import dev.mobilewebcambridge.protocol.session.ConnectionId
import dev.mobilewebcambridge.protocol.session.HeartbeatWatchdog
import dev.mobilewebcambridge.protocol.session.SessionEffect
import dev.mobilewebcambridge.protocol.session.SessionEvent
import dev.mobilewebcambridge.protocol.session.SessionReducer
import dev.mobilewebcambridge.protocol.session.SessionState
import dev.mobilewebcambridge.protocol.time.MonotonicClock
import dev.mobilewebcambridge.protocol.transport.OutboundClass
import dev.mobilewebcambridge.protocol.transport.OutboundItem
import dev.mobilewebcambridge.protocol.transport.SendScheduler
import dev.mobilewebcambridge.protocol.wire.Packet
import dev.mobilewebcambridge.protocol.wire.PacketStreamParser
import dev.mobilewebcambridge.protocol.wire.PacketType
import dev.mobilewebcambridge.protocol.wire.ProtocolException
import java.net.Socket
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Runs the device side of the wire protocol (same design as the iOS `SessionCoordinator`).
 *
 * Everything happens on [thread]: connection events, packet parsing, the session reducer, the send
 * scheduler and the 100 ms tick. Media and system components hand immutable values over with
 * `post`, which keeps their order.
 */
class SessionCoordinator(
    private val thread: SerialThread,
    private val identity: AppIdentity,
    private val clock: MonotonicClock,
    private val logger: AppLogger,
    private val diagnostics: DiagnosticsSource,
    private val bootReport: List<String>,
) : MediaEventSink, ConnectionListener {
    private val snapshots = MutableStateFlow(SessionSnapshot())

    /** For the UI; updated at most twice a second, immediately on connection changes. */
    val snapshot: StateFlow<SessionSnapshot> = snapshots.asStateFlow()

    // Session-thread state.
    private var media: MediaControlling? = null
    private var state = SessionState()
    private val pendingEvents = ArrayDeque<SessionEvent>()
    private var applying = false
    private val drivers = HashMap<ConnectionId, ConnectionDriver>()
    private var activeConnection: ConnectionId? = null
    private var nextConnectionNumber = 1L
    private var parser = PacketStreamParser()
    private val scheduler = SendScheduler()
    private var factory = OutboundPacketFactory()
    private val watchdog = HeartbeatWatchdog()
    private val clockOffset = ClockOffsetEstimator()
    private var currentVideoConfigId: Int? = null
    private var currentAudioConfigId: Int? = null
    private var host: String? = null
    private var videoStatus = VideoStreamStatus.OFF
    private var audioStatus = AudioStreamStatus.OFF
    private var system = SystemSnapshot()
    private var serverState: CompanionServer.State = CompanionServer.State.Starting
    private var lastSentStatus: StatusMessage? = null
    private var lastStatusSentUs = 0L
    private val traffic = TrafficStats()
    private var lastSnapshotUs = 0L
    private var ticking = false
    private val tick = Runnable { onTick() }

    /** Breaks the construction cycle: the media pipelines report to this object. */
    fun bind(media: MediaControlling) = thread.post { this.media = media }

    fun start() = thread.post {
        if (ticking) return@post
        ticking = true
        thread.postDelayed(TICK_MS, tick)
        publishSnapshot(force = true)
    }

    /** Closes the host connection and stops the media; the session can be started again. */
    fun shutdown() = thread.post {
        ticking = false
        thread.cancel(tick)
        drivers.values.forEach { it.close("The app stopped streaming") }
        activeConnection?.let { id ->
            activeConnection = null
            resetTransportState()
            dispatch(SessionEvent.ConnectionClosed(id))
        }
        serverState = CompanionServer.State.Stopped
        publishSnapshot(force = true)
    }

    // ---- Inputs from the server and the system monitor (any thread) ----------------------------

    fun serverStateChanged(newState: CompanionServer.State) = thread.post {
        serverState = newState
        publishSnapshot(force = true)
    }

    /** Called on the server's accept thread. */
    fun accept(socket: Socket) = thread.post {
        val id = ConnectionId(nextConnectionNumber++)
        val driver = ConnectionDriver(id, socket, thread, this)
        drivers[id] = driver
        logger.info("transport", "Host connecting from ${driver.remoteDescription} ($id)")
        driver.start()
    }

    fun updateSystem(snapshot: SystemSnapshot) = thread.post {
        if (snapshot == system) return@post
        system = snapshot
        sendStatus(force = false)
    }

    // ---- ConnectionListener (session thread) ---------------------------------------------------

    override fun connectionOpened(id: ConnectionId) {
        if (id !in drivers) return
        logger.info("transport", "Connection $id open")
        activeConnection = id
        resetTransportState()
        dispatch(SessionEvent.ConnectionOpened(id))
        publishSnapshot(force = true)
    }

    override fun connectionReceived(id: ConnectionId, bytes: ByteArray) {
        if (id != activeConnection) return
        val receivedAt = clock.nowMicros()
        val packets: List<Packet> = try {
            parser.push(bytes)
        } catch (error: ProtocolException) {
            logger.error("session", "Protocol violation: ${error.code}")
            dispatch(SessionEvent.ProtocolViolation(id, error))
            return
        }
        for (packet in packets) {
            if (id != activeConnection) return
            watchdog.noteInbound(receivedAt)
            val command = try {
                HostCommandDecoder.decode(packet)
            } catch (error: MessageDecodingException) {
                logger.warn("session", "Rejected ${error.type.name}: ${error.detail}")
                dispatch(SessionEvent.CommandRejected(id, error))
                continue
            } catch (error: ProtocolException) {
                dispatch(SessionEvent.ProtocolViolation(id, error))
                return
            }
            if (command is HostCommand.Hello) {
                host = "${command.message.app.name} ${command.message.app.version}"
            }
            dispatch(SessionEvent.CommandReceived(id, command, receivedAt))
        }
    }

    override fun connectionSent(id: ConnectionId, byteCount: Int) {
        if (id != activeConnection) return
        scheduler.acknowledge(byteCount)
        pump()
    }

    override fun connectionClosed(id: ConnectionId, reason: String) {
        drivers.remove(id)
        logger.info("transport", "Connection $id closed: $reason")
        if (id != activeConnection) return
        activeConnection = null
        resetTransportState()
        dispatch(SessionEvent.ConnectionClosed(id))
        publishSnapshot(force = true)
    }

    // ---- MediaEventSink (any thread) -----------------------------------------------------------

    override fun videoConfigured(config: VideoConfigMessage) = thread.post {
        if (state.readyConnection == null || state.video == null) return@post
        currentVideoConfigId = config.configId
        // Queued frames belong to the previous configuration (and control would overtake them).
        scheduler.purgeVideo()
        logger.info(
            "session",
            "VideoConfig ${config.configId}: ${config.width}x${config.height}@${config.fps} ${config.bitrateKbps} kbps " +
                "${config.profile} ${config.encoder.wire}, ${config.camera.wire}, rotation ${config.rotationDeg}",
        )
        sendJson(PacketType.VIDEO_CONFIG, config, OutboundClass.Control)
    }

    override fun videoEncoded(frame: EncodedVideoFrame) = thread.post {
        if (state.readyConnection == null || frame.configId != currentVideoConfigId) return@post
        val packet = try {
            factory.videoAccessUnit(frame)
        } catch (error: OutboundPacketFactory.PayloadTooLargeException) {
            logger.warn("session", "Dropped an oversized access unit (${frame.annexB.size} bytes)")
            media?.requestKeyframe()
            return@post
        }
        traffic.record(frame.annexB.size)
        enqueue(packet, OutboundClass.Video(isKeyframe = frame.isKeyframe, isDisposable = frame.isDisposable))
    }

    override fun videoStatusChanged(status: VideoStreamStatus) = thread.post {
        videoStatus = status
        sendStatus(force = false)
        publishSnapshot(force = true)
    }

    override fun videoFailed(error: ErrorMessage) = thread.post { dispatch(SessionEvent.VideoFailed(error)) }

    override fun audioConfigured(config: AudioConfigMessage) = thread.post {
        if (state.readyConnection == null || state.audio == null) return@post
        currentAudioConfigId = config.configId
        sendJson(PacketType.AUDIO_CONFIG, config, OutboundClass.Control)
    }

    override fun audioCaptured(chunk: AudioChunk) = thread.post {
        if (state.readyConnection == null || chunk.configId != currentAudioConfigId) return@post
        val packet = try {
            factory.audioChunk(chunk)
        } catch (error: OutboundPacketFactory.PayloadTooLargeException) {
            logger.warn("session", "Dropped an oversized audio chunk (${chunk.pcm.size} bytes)")
            return@post
        }
        enqueue(packet, OutboundClass.Audio)
    }

    override fun audioStatusChanged(status: AudioStreamStatus) = thread.post {
        audioStatus = status
        sendStatus(force = false)
        publishSnapshot(force = true)
    }

    override fun audioFailed(error: ErrorMessage) = thread.post { dispatch(SessionEvent.AudioFailed(error)) }

    // ---- Reducer ---------------------------------------------------------------------------------

    /** Run-to-completion: events raised while effects execute are queued, never nested. */
    private fun dispatch(event: SessionEvent) {
        pendingEvents.addLast(event)
        if (applying) return
        applying = true
        try {
            while (pendingEvents.isNotEmpty()) {
                val transition = SessionReducer.reduce(state, pendingEvents.removeFirst())
                state = transition.state
                transition.effects.forEach { execute(it) }
            }
        } finally {
            applying = false
        }
    }

    private fun execute(effect: SessionEffect) {
        when (effect) {
            is SessionEffect.SendHello -> {
                if (effect.id != activeConnection) return
                sendJson(PacketType.HELLO, identity.hello, OutboundClass.Control)
            }
            is SessionEffect.SendError -> {
                if (effect.id != activeConnection) return
                logger.warn("session", "Sending error ${effect.message.code.raw}: ${effect.message.message}")
                sendJson(PacketType.ERROR, effect.message, OutboundClass.Control)
            }
            is SessionEffect.SendPong -> {
                if (effect.id != activeConnection) return
                enqueue(factory.pong(effect.payload, clock.nowMicros()), OutboundClass.Control)
            }
            is SessionEffect.RecordClockSample -> clockOffset.add(effect.t0, effect.t1, effect.t2, effect.t3)
            is SessionEffect.HandshakeCompleted -> {
                if (effect.id != activeConnection) return
                logger.info("session", "Host handshake complete (protocol 1.${effect.effectiveMinor}, ${host ?: "unknown host"})")
                watchdog.start(clock.nowMicros())
                sendBootReport()
                sendStatus(force = true)
                publishSnapshot(force = true)
            }
            is SessionEffect.CloseConnection -> {
                logger.info("session", "Closing connection ${effect.id}: ${effect.reason}")
                if (effect.id == activeConnection) {
                    activeConnection = null
                    resetTransportState()
                }
                // Posted so the driver's close callback never re-enters the reducer; data handed
                // to the driver before (e.g. a fatal Error) is still written.
                drivers[effect.id]?.let { driver -> thread.post { driver.close(effect.reason.toString()) } }
            }
            is SessionEffect.StartVideo -> {
                val request = effect.request
                currentVideoConfigId = null
                scheduler.purgeVideo()
                logger.info(
                    "session",
                    "Host requested video ${request.width}x${request.height}@${request.fps} ${request.bitrateKbps} kbps, " +
                        "${request.camera.wire}, ${request.orientation.wire}, ${request.encoder.wire}",
                )
                media?.startVideo(request)
            }
            SessionEffect.StopVideo -> {
                currentVideoConfigId = null
                scheduler.purgeVideo()
                logger.info("session", "Video stopped")
                media?.stopVideo()
            }
            SessionEffect.RequestKeyframe -> media?.requestKeyframe()
            is SessionEffect.StartAudio -> {
                currentAudioConfigId = null
                logger.info("session", "Host requested audio (${effect.request.processing.wire})")
                media?.startAudio(effect.request)
            }
            SessionEffect.StopAudio -> {
                currentAudioConfigId = null
                logger.info("session", "Audio stopped")
                media?.stopAudio()
            }
        }
    }

    // ---- Sending ---------------------------------------------------------------------------------

    private fun sendJson(type: PacketType, message: JsonMessage, kind: OutboundClass) {
        val packet = try {
            factory.json(type, message, clock.nowMicros())
        } catch (error: OutboundPacketFactory.PayloadTooLargeException) {
            logger.error("session", "Cannot send ${type.name}: ${error.message}")
            return
        }
        enqueue(packet, kind)
    }

    private fun enqueue(packet: Packet, kind: OutboundClass) {
        val id = activeConnection ?: return
        val result = scheduler.enqueue(OutboundItem(packet, kind))
        traffic.droppedVideoFrames += result.droppedVideoFrames
        if (result.keyframeNeeded) media?.requestKeyframe()
        if (result.audioBacklogExceeded) {
            logger.error("session", "More than one second of audio is queued; closing the connection")
            dispatch(SessionEvent.AudioBacklogExceeded(id))
            return
        }
        pump()
    }

    private fun pump() {
        val driver = activeConnection?.let { drivers[it] } ?: return
        while (true) {
            val item = scheduler.dequeue() ?: return
            driver.send(item.bytes)
        }
    }

    private fun sendLog(entry: LogEntry) {
        // Never log from here: a failure would feed itself through the forwarding ring.
        val packet = runCatching { factory.json(PacketType.LOG, entry.logMessage.truncated(), entry.timestampUs) }.getOrNull() ?: return
        enqueue(packet, OutboundClass.Log)
    }

    private fun forwardLogs() {
        val batch = logger.takeForwardingBatch(LOGS_PER_TICK)
        if (batch.dropped > 0) {
            sendLog(LogEntry(clock.nowMicros(), LogLevel.WARN, "log", "${batch.dropped} log entries were dropped before forwarding"))
        }
        batch.entries.forEach { sendLog(it) }
    }

    private fun sendBootReport() {
        val now = clock.nowMicros()
        val app = identity.app
        val device = identity.device
        val lines = listOf("${device.model}, ${device.os}, app ${app.version} (${app.build}), git ${app.gitSha}") + bootReport
        lines.forEach { sendLog(LogEntry(now, LogLevel.INFO, "boot", it)) }
        diagnostics.takePendingReports(limit = 5).forEach { sendLog(LogEntry(now, LogLevel.WARN, "diagnostics", it)) }
    }

    private fun sendStatus(force: Boolean) {
        if (state.readyConnection == null) return
        val status = StatusMessage(
            appState = system.appState,
            audio = audioStatus,
            battery = system.battery,
            lowPower = system.lowPower,
            permissions = system.permissions,
            thermal = system.thermal.statusName,
            video = videoStatus,
        )
        if (!force && status == lastSentStatus) return
        lastSentStatus = status
        lastStatusSentUs = clock.nowMicros()
        sendJson(PacketType.STATUS, status, OutboundClass.Control)
    }

    // ---- Timer -----------------------------------------------------------------------------------

    private fun onTick() {
        if (!ticking) return
        val now = clock.nowMicros()
        state.readyConnection?.let { id ->
            when (watchdog.tick(now)) {
                HeartbeatWatchdog.Action.SEND_PING -> enqueue(factory.ping(now), OutboundClass.Control)
                HeartbeatWatchdog.Action.TIMED_OUT -> {
                    logger.warn("session", "No packet from the host for 4 s")
                    dispatch(SessionEvent.HeartbeatTimedOut(id))
                }
                null -> Unit
            }
        }
        if (state.readyConnection != null) {
            forwardLogs()
            if (now - lastStatusSentUs >= STATUS_INTERVAL_US) sendStatus(force = true)
        }
        traffic.roll(now)
        publishSnapshot(force = false)
        thread.postDelayed(TICK_MS, tick)
    }

    // ---- State helpers ---------------------------------------------------------------------------

    private fun resetTransportState() {
        parser = PacketStreamParser()
        scheduler.reset()
        factory = OutboundPacketFactory()
        watchdog.stop()
        clockOffset.reset()
        currentVideoConfigId = null
        currentAudioConfigId = null
        lastSentStatus = null
        host = null
    }

    private val connectionStatus: ConnectionStatus
        get() = when (state.phase) {
            is SessionState.Phase.Ready -> ConnectionStatus.Connected
            is SessionState.Phase.AwaitingHello -> ConnectionStatus.Handshaking
            SessionState.Phase.Idle -> when (val server = serverState) {
                CompanionServer.State.Starting -> ConnectionStatus.Starting
                is CompanionServer.State.Listening -> ConnectionStatus.Listening(server.port)
                is CompanionServer.State.Failed -> ConnectionStatus.ListenerFailed(server.message)
                CompanionServer.State.Stopped -> ConnectionStatus.Stopped
            }
        }

    private fun publishSnapshot(force: Boolean) {
        val now = clock.nowMicros()
        if (!force && now - lastSnapshotUs < SNAPSHOT_INTERVAL_US) return
        lastSnapshotUs = now
        snapshots.value = SessionSnapshot(
            connection = connectionStatus,
            host = host.takeIf { state.readyConnection != null },
            video = videoStatus,
            audio = audioStatus,
            roundTripMs = clockOffset.best?.let { (it.roundTripUs / 1_000).toInt() },
            videoFps = traffic.framesPerSecond,
            videoKbps = traffic.kilobitsPerSecond,
            droppedVideoFrames = traffic.droppedVideoFrames,
        )
    }

    private companion object {
        const val TICK_MS = 100L
        const val STATUS_INTERVAL_US = 5_000_000L
        const val SNAPSHOT_INTERVAL_US = 500_000L

        /** 10 per 100 ms tick: at most 100 forwarded log entries per second. */
        const val LOGS_PER_TICK = 10
    }
}

/** Outgoing video rate over the last second, for the diagnostics screen. */
private class TrafficStats {
    private var frames = 0
    private var bytes = 0L
    private var windowStartUs = 0L
    var framesPerSecond = 0
        private set
    var kilobitsPerSecond = 0
        private set
    var droppedVideoFrames = 0

    fun record(videoBytes: Int) {
        frames += 1
        bytes += videoBytes
    }

    fun roll(nowUs: Long) {
        val elapsed = nowUs - windowStartUs
        if (elapsed < 1_000_000) return
        framesPerSecond = (frames * 1_000_000L / elapsed).toInt()
        kilobitsPerSecond = (bytes * 8_000 / elapsed).toInt()
        frames = 0
        bytes = 0
        windowStartUs = nowUs
    }
}
