package dev.mobilewebcambridge.protocol.session

import dev.mobilewebcambridge.protocol.messages.AppDescriptor
import dev.mobilewebcambridge.protocol.messages.AudioProcessing
import dev.mobilewebcambridge.protocol.messages.CameraSelection
import dev.mobilewebcambridge.protocol.messages.EncoderMode
import dev.mobilewebcambridge.protocol.messages.ErrorCode
import dev.mobilewebcambridge.protocol.messages.ErrorMessage
import dev.mobilewebcambridge.protocol.messages.HelloMessage
import dev.mobilewebcambridge.protocol.messages.HostCommand
import dev.mobilewebcambridge.protocol.messages.OrientationMode
import dev.mobilewebcambridge.protocol.messages.PeerRole
import dev.mobilewebcambridge.protocol.messages.PongPayload
import dev.mobilewebcambridge.protocol.messages.ProtocolVersion
import dev.mobilewebcambridge.protocol.messages.StartAudioMessage
import dev.mobilewebcambridge.protocol.messages.StartVideoMessage
import dev.mobilewebcambridge.protocol.wire.PacketType
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertNull
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test

class SessionReducerTest {
    private val first = ConnectionId(1)
    private val second = ConnectionId(2)
    private val video = StartVideoMessage(1280, 720, 30, 6000, CameraSelection.BACK_WIDE, false, OrientationMode.AUTO, EncoderMode.LOW_LATENCY)
    private val audio = StartAudioMessage(AudioProcessing.STANDARD)

    private fun hostHello(major: Int = 1, minor: Int = 0, role: PeerRole = PeerRole.HOST) =
        HostCommand.Hello(HelloMessage(ProtocolVersion(major, minor), role, AppDescriptor("host", "1", "1", "x"), null, emptyList()))

    private fun run(vararg events: SessionEvent, from: SessionState = SessionState()): Pair<SessionState, List<SessionEffect>> {
        var state = from
        val effects = ArrayList<SessionEffect>()
        for (event in events) {
            val transition = SessionReducer.reduce(state, event)
            state = transition.state
            effects += transition.effects
        }
        return state to effects
    }

    private fun received(command: HostCommand, id: ConnectionId = first, at: Long = 0) = SessionEvent.CommandReceived(id, command, at)

    @Test
    fun `sends Hello on connect and becomes ready on the host Hello`() {
        val (state, effects) = run(SessionEvent.ConnectionOpened(first), received(hostHello(minor = 3)))
        assertEquals(SessionState.Phase.Ready(first, effectiveMinor = 0), state.phase)
        assertEquals(listOf(SessionEffect.SendHello(first), SessionEffect.HandshakeCompleted(first, 0)), effects)
    }

    @Test
    fun `ignores commands before the host Hello`() {
        val (state, effects) = run(SessionEvent.ConnectionOpened(first), received(HostCommand.StartVideo(video)))
        assertNull(state.video)
        assertEquals(listOf(SessionEffect.SendHello(first)), effects)
    }

    @Test
    fun `rejects a different major version`() {
        val (state, effects) = run(SessionEvent.ConnectionOpened(first), received(hostHello(major = 2)))
        assertEquals(SessionState.Phase.Idle, state.phase)
        assertTrue(effects.any { it is SessionEffect.SendError && it.message.code == ErrorCode.VERSION_MISMATCH && it.message.fatal })
        assertEquals(SessionEffect.CloseConnection(first, CloseReason.VersionMismatch), effects.last())
    }

    @Test
    fun `starts streams idempotently and stops them`() {
        val (state, effects) = run(
            SessionEvent.ConnectionOpened(first),
            received(hostHello()),
            received(HostCommand.StartVideo(video)),
            received(HostCommand.StartVideo(video)),
            received(HostCommand.StartAudio(audio)),
            received(HostCommand.RequestKeyframe),
            received(HostCommand.StopVideo),
            received(HostCommand.StopVideo),
        )
        assertNull(state.video)
        assertEquals(audio, state.audio)
        assertEquals(
            listOf(
                SessionEffect.StartVideo(video),
                SessionEffect.StartAudio(audio),
                SessionEffect.RequestKeyframe,
                SessionEffect.StopVideo,
            ),
            effects.drop(2),
        )
    }

    @Test
    fun `answers pings and records pong samples`() {
        val (_, effects) = run(
            SessionEvent.ConnectionOpened(first),
            received(hostHello()),
            received(HostCommand.Ping(t0 = 100), at = 150),
            received(HostCommand.Pong(PongPayload(echoT0 = 10, t1 = 20), sentAtUs = 25), at = 40),
        )
        assertEquals(SessionEffect.SendPong(first, PongPayload(echoT0 = 100, t1 = 150)), effects[2])
        assertEquals(SessionEffect.RecordClockSample(t0 = 10, t1 = 20, t2 = 25, t3 = 40), effects[3])
    }

    @Test
    fun `a new connection replaces the old one and stops its streams`() {
        val (state, effects) = run(
            SessionEvent.ConnectionOpened(first),
            received(hostHello()),
            received(HostCommand.StartVideo(video)),
            SessionEvent.ConnectionOpened(second),
        )
        assertEquals(SessionState.Phase.AwaitingHello(second), state.phase)
        assertEquals(
            listOf(SessionEffect.CloseConnection(first, CloseReason.Replaced), SessionEffect.StopVideo, SessionEffect.SendHello(second)),
            effects.drop(3),
        )
    }

    @Test
    fun `events of a replaced connection are ignored`() {
        val (state, effects) = run(
            SessionEvent.ConnectionOpened(first),
            SessionEvent.ConnectionOpened(second),
            SessionEvent.HeartbeatTimedOut(first),
            SessionEvent.ConnectionClosed(first),
        )
        assertEquals(SessionState.Phase.AwaitingHello(second), state.phase)
        assertEquals(3, effects.size)
    }

    @Test
    fun `heartbeat timeout and audio backlog tear the connection down`() {
        val (_, heartbeat) = run(SessionEvent.ConnectionOpened(first), received(hostHello()), SessionEvent.HeartbeatTimedOut(first))
        assertEquals(SessionEffect.CloseConnection(first, CloseReason.HeartbeatTimeout), heartbeat.last())
        val (_, backlog) = run(
            SessionEvent.ConnectionOpened(first),
            received(hostHello()),
            received(HostCommand.StartAudio(audio)),
            SessionEvent.AudioBacklogExceeded(first),
        )
        assertEquals(listOf(SessionEffect.StopAudio, SessionEffect.CloseConnection(first, CloseReason.AudioBacklog)), backlog.takeLast(2))
    }

    @Test
    fun `protocol violations send a fatal error and close`() {
        val (_, effects) = run(SessionEvent.ConnectionOpened(first), SessionEvent.ProtocolViolation(first, "BAD_MAGIC"))
        assertEquals(
            listOf(
                SessionEffect.SendHello(first),
                SessionEffect.SendError(first, ErrorMessage(ErrorCode.BAD_REQUEST, "Protocol violation: BAD_MAGIC", fatal = true)),
                SessionEffect.CloseConnection(first, CloseReason.ProtocolViolation("BAD_MAGIC")),
            ),
            effects,
        )
    }

    @Test
    fun `invalid commands after the handshake are rejected without closing`() {
        val (state, effects) = run(
            SessionEvent.ConnectionOpened(first),
            received(hostHello()),
            SessionEvent.CommandRejected(first, PacketType.START_VIDEO, "fps out of range"),
        )
        assertEquals(SessionState.Phase.Ready(first, 0), state.phase)
        val error = effects.last() as SessionEffect.SendError
        assertEquals(false, error.message.fatal)
    }

    @Test
    fun `media failures clear the request and report to a ready host`() {
        val failure = ErrorMessage(ErrorCode.CAMERA_UNAVAILABLE, "busy", fatal = false)
        val (state, effects) = run(
            SessionEvent.ConnectionOpened(first),
            received(hostHello()),
            received(HostCommand.StartVideo(video)),
            SessionEvent.VideoFailed(failure),
        )
        assertNull(state.video)
        assertEquals(SessionEffect.SendError(first, failure), effects.last())
    }
}

class HeartbeatWatchdogTest {
    @Test
    fun `pings every interval and times out after silence`() {
        val watchdog = HeartbeatWatchdog(pingIntervalUs = 1_000_000, timeoutUs = 4_000_000)
        assertNull(watchdog.tick(0), "not running before start")
        watchdog.start(0)
        assertEquals(HeartbeatWatchdog.Action.SEND_PING, watchdog.tick(100_000))
        assertNull(watchdog.tick(500_000))
        assertEquals(HeartbeatWatchdog.Action.SEND_PING, watchdog.tick(1_100_000))
        watchdog.noteInbound(3_000_000)
        assertEquals(HeartbeatWatchdog.Action.SEND_PING, watchdog.tick(6_900_000))
        assertEquals(HeartbeatWatchdog.Action.TIMED_OUT, watchdog.tick(7_000_000))
        assertNull(watchdog.tick(8_000_000), "stopped after the timeout")
    }
}

class ClockOffsetEstimatorTest {
    @Test
    fun `keeps the minimum round trip sample`() {
        val estimator = ClockOffsetEstimator(windowSize = 4)
        assertNull(estimator.best)
        // Peer clock = local + 5000 µs; symmetric 1 ms path, 100 µs processing.
        estimator.add(t0 = 0, t1 = 6_000, t2 = 6_100, t3 = 2_100)
        // A congested exchange that must not win.
        estimator.add(t0 = 10_000, t1 = 25_000, t2 = 25_100, t3 = 21_100)
        assertEquals(ClockSample(roundTripUs = 2_000, offsetUs = 5_000), estimator.best)
    }

    @Test
    fun `ignores impossible samples`() {
        val estimator = ClockOffsetEstimator()
        assertNull(estimator.add(t0 = 100, t1 = 0, t2 = 1_000, t3 = 50))
        assertNull(estimator.best)
    }
}
