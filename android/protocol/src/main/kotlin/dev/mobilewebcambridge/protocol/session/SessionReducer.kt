package dev.mobilewebcambridge.protocol.session

import dev.mobilewebcambridge.protocol.messages.ErrorCode
import dev.mobilewebcambridge.protocol.messages.ErrorMessage
import dev.mobilewebcambridge.protocol.messages.HostCommand
import dev.mobilewebcambridge.protocol.messages.MessageDecodingException
import dev.mobilewebcambridge.protocol.messages.PeerRole
import dev.mobilewebcambridge.protocol.messages.PongPayload
import dev.mobilewebcambridge.protocol.messages.ProtocolVersion
import dev.mobilewebcambridge.protocol.messages.StartAudioMessage
import dev.mobilewebcambridge.protocol.messages.StartVideoMessage
import dev.mobilewebcambridge.protocol.wire.PacketType
import dev.mobilewebcambridge.protocol.wire.ProtocolException

/** Identifies one accepted TCP connection; a new connection replaces the previous one. */
@JvmInline
value class ConnectionId(val value: Long) {
    override fun toString(): String = "#$value"
}

sealed interface CloseReason {
    data object Replaced : CloseReason

    data object VersionMismatch : CloseReason

    data class BadHandshake(val detail: String) : CloseReason

    data object HeartbeatTimeout : CloseReason

    data class ProtocolViolation(val code: String) : CloseReason

    data class PeerError(val code: ErrorCode) : CloseReason

    data object AudioBacklog : CloseReason
}

/** Device-side session state. Pure value: all side effects are described by [SessionEffect]. */
data class SessionState(
    val phase: Phase = Phase.Idle,
    /** Video the host asked for (null = stopped). */
    val video: StartVideoMessage? = null,
    /** Audio the host asked for (null = stopped). */
    val audio: StartAudioMessage? = null,
) {
    sealed interface Phase {
        /** No host connection. */
        data object Idle : Phase

        /** Connected; our Hello is sent, the host's has not arrived yet. */
        data class AwaitingHello(val connection: ConnectionId) : Phase

        /** Handshake complete. */
        data class Ready(val connection: ConnectionId, val effectiveMinor: Int) : Phase
    }

    val connection: ConnectionId?
        get() = when (phase) {
            Phase.Idle -> null
            is Phase.AwaitingHello -> phase.connection
            is Phase.Ready -> phase.connection
        }

    val readyConnection: ConnectionId? get() = (phase as? Phase.Ready)?.connection
}

sealed interface SessionEvent {
    data class ConnectionOpened(val id: ConnectionId) : SessionEvent

    data class ConnectionClosed(val id: ConnectionId) : SessionEvent

    data class CommandReceived(val id: ConnectionId, val command: HostCommand, val receivedAtUs: Long) : SessionEvent

    data class CommandRejected(val id: ConnectionId, val type: PacketType, val detail: String) : SessionEvent {
        constructor(id: ConnectionId, error: MessageDecodingException) : this(id, error.type, error.detail)
    }

    data class ProtocolViolation(val id: ConnectionId, val code: String) : SessionEvent {
        constructor(id: ConnectionId, error: ProtocolException) : this(id, error.code)
    }

    data class HeartbeatTimedOut(val id: ConnectionId) : SessionEvent

    data class AudioBacklogExceeded(val id: ConnectionId) : SessionEvent

    data class VideoFailed(val error: ErrorMessage) : SessionEvent

    data class AudioFailed(val error: ErrorMessage) : SessionEvent
}

sealed interface SessionEffect {
    data class SendHello(val id: ConnectionId) : SessionEffect

    data class SendError(val id: ConnectionId, val message: ErrorMessage) : SessionEffect

    data class SendPong(val id: ConnectionId, val payload: PongPayload) : SessionEffect

    data class RecordClockSample(val t0: Long, val t1: Long, val t2: Long, val t3: Long) : SessionEffect

    data class HandshakeCompleted(val id: ConnectionId, val effectiveMinor: Int) : SessionEffect

    data class CloseConnection(val id: ConnectionId, val reason: CloseReason) : SessionEffect

    data class StartVideo(val request: StartVideoMessage) : SessionEffect

    data object StopVideo : SessionEffect

    data object RequestKeyframe : SessionEffect

    data class StartAudio(val request: StartAudioMessage) : SessionEffect

    data object StopAudio : SessionEffect
}

data class SessionTransition(val state: SessionState, val effects: List<SessionEffect>)

/**
 * The device session state machine (SPEC §2), identical to the iOS `SessionReducer`. Media streams
 * only run while a handshaken host is connected; losing the connection stops them, and the host
 * replays its desired streams after reconnecting.
 */
object SessionReducer {
    val localVersion: ProtocolVersion = ProtocolVersion.CURRENT

    fun reduce(state: SessionState, event: SessionEvent): SessionTransition {
        val builder = Builder(state)
        builder.apply(event)
        return SessionTransition(builder.state, builder.effects)
    }

    private class Builder(var state: SessionState) {
        val effects = ArrayList<SessionEffect>()

        fun apply(event: SessionEvent) {
            when (event) {
                is SessionEvent.ConnectionOpened -> {
                    val current = state.connection
                    if (current != null && current != event.id) effects += SessionEffect.CloseConnection(current, CloseReason.Replaced)
                    stopStreams()
                    state = state.copy(phase = SessionState.Phase.AwaitingHello(event.id))
                    effects += SessionEffect.SendHello(event.id)
                }
                is SessionEvent.ConnectionClosed -> {
                    if (state.connection != event.id) return
                    stopStreams()
                    state = state.copy(phase = SessionState.Phase.Idle)
                }
                is SessionEvent.HeartbeatTimedOut -> {
                    if (state.connection != event.id) return
                    tearDown(event.id, CloseReason.HeartbeatTimeout)
                }
                is SessionEvent.AudioBacklogExceeded -> {
                    if (state.connection != event.id) return
                    tearDown(event.id, CloseReason.AudioBacklog)
                }
                is SessionEvent.ProtocolViolation -> {
                    if (state.connection != event.id) return
                    effects += SessionEffect.SendError(
                        event.id,
                        ErrorMessage(ErrorCode.BAD_REQUEST, "Protocol violation: ${event.code}", fatal = true),
                    )
                    tearDown(event.id, CloseReason.ProtocolViolation(event.code))
                }
                is SessionEvent.CommandRejected -> {
                    if (state.connection != event.id) return
                    rejected(event, event.id)
                }
                is SessionEvent.CommandReceived -> {
                    if (state.connection != event.id) return
                    when (state.phase) {
                        SessionState.Phase.Idle -> return
                        is SessionState.Phase.AwaitingHello -> beforeHello(event.command, event.id)
                        is SessionState.Phase.Ready -> whileReady(event.command, event.id, event.receivedAtUs)
                    }
                }
                is SessionEvent.VideoFailed -> {
                    state = state.copy(video = null)
                    state.readyConnection?.let { effects += SessionEffect.SendError(it, event.error) }
                }
                is SessionEvent.AudioFailed -> {
                    state = state.copy(audio = null)
                    state.readyConnection?.let { effects += SessionEffect.SendError(it, event.error) }
                }
            }
        }

        private fun beforeHello(command: HostCommand, id: ConnectionId) {
            when (command) {
                is HostCommand.Hello -> {
                    val hello = command.message
                    if (hello.protocolVersion.major != localVersion.major) {
                        val message = "Unsupported protocol major version ${hello.protocolVersion.major}"
                        effects += SessionEffect.SendError(id, ErrorMessage(ErrorCode.VERSION_MISMATCH, message, fatal = true))
                        tearDown(id, CloseReason.VersionMismatch)
                        return
                    }
                    if (hello.role != PeerRole.HOST) {
                        effects += SessionEffect.SendError(id, ErrorMessage(ErrorCode.BAD_REQUEST, "Expected role host", fatal = true))
                        tearDown(id, CloseReason.BadHandshake("peer role is ${hello.role.wire}"))
                        return
                    }
                    val minor = minOf(localVersion.minor, hello.protocolVersion.minor)
                    state = state.copy(phase = SessionState.Phase.Ready(id, minor))
                    effects += SessionEffect.HandshakeCompleted(id, minor)
                }
                is HostCommand.Error -> if (command.message.fatal) tearDown(id, CloseReason.PeerError(command.message.code))
                // SPEC §2.3: everything else before the host Hello is ignored.
                else -> Unit
            }
        }

        private fun whileReady(command: HostCommand, id: ConnectionId, receivedAtUs: Long) {
            when (command) {
                is HostCommand.Hello, is HostCommand.Ignored -> Unit
                is HostCommand.Error -> if (command.message.fatal) tearDown(id, CloseReason.PeerError(command.message.code))
                is HostCommand.Ping -> effects += SessionEffect.SendPong(id, PongPayload(echoT0 = command.t0, t1 = receivedAtUs))
                is HostCommand.Pong ->
                    effects += SessionEffect.RecordClockSample(command.payload.echoT0, command.payload.t1, command.sentAtUs, receivedAtUs)
                is HostCommand.StartVideo -> {
                    if (state.video == command.message) return
                    state = state.copy(video = command.message)
                    effects += SessionEffect.StartVideo(command.message)
                }
                HostCommand.StopVideo -> {
                    if (state.video == null) return
                    state = state.copy(video = null)
                    effects += SessionEffect.StopVideo
                }
                HostCommand.RequestKeyframe -> if (state.video != null) effects += SessionEffect.RequestKeyframe
                is HostCommand.StartAudio -> {
                    if (state.audio == command.message) return
                    state = state.copy(audio = command.message)
                    effects += SessionEffect.StartAudio(command.message)
                }
                HostCommand.StopAudio -> {
                    if (state.audio == null) return
                    state = state.copy(audio = null)
                    effects += SessionEffect.StopAudio
                }
            }
        }

        private fun rejected(event: SessionEvent.CommandRejected, id: ConnectionId) {
            when (state.phase) {
                SessionState.Phase.Idle -> return
                is SessionState.Phase.AwaitingHello -> {
                    if (event.type != PacketType.HELLO) return
                    effects += SessionEffect.SendError(
                        id,
                        ErrorMessage(ErrorCode.BAD_REQUEST, "Invalid Hello: ${event.detail}", fatal = true),
                    )
                    tearDown(id, CloseReason.BadHandshake(event.detail))
                }
                is SessionState.Phase.Ready -> effects += SessionEffect.SendError(
                    id,
                    ErrorMessage(ErrorCode.BAD_REQUEST, "Invalid ${event.type.name}: ${event.detail}", fatal = false),
                )
            }
        }

        private fun tearDown(id: ConnectionId, reason: CloseReason) {
            stopStreams()
            state = state.copy(phase = SessionState.Phase.Idle)
            effects += SessionEffect.CloseConnection(id, reason)
        }

        private fun stopStreams() {
            if (state.video != null) {
                state = state.copy(video = null)
                effects += SessionEffect.StopVideo
            }
            if (state.audio != null) {
                state = state.copy(audio = null)
                effects += SessionEffect.StopAudio
            }
        }
    }
}
