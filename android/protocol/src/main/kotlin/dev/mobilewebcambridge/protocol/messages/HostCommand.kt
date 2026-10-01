package dev.mobilewebcambridge.protocol.messages

import dev.mobilewebcambridge.protocol.json.JsonParser
import dev.mobilewebcambridge.protocol.json.JsonSyntaxException
import dev.mobilewebcambridge.protocol.wire.Packet
import dev.mobilewebcambridge.protocol.wire.PacketType

/** A packet from the host, decoded into what the device session acts on. */
sealed interface HostCommand {
    data class Hello(val message: HelloMessage) : HostCommand

    data class Error(val message: ErrorMessage) : HostCommand

    data class Ping(val t0: Long) : HostCommand

    data class Pong(val payload: PongPayload, val sentAtUs: Long) : HostCommand

    data class StartVideo(val message: StartVideoMessage) : HostCommand

    data object StopVideo : HostCommand

    data object RequestKeyframe : HostCommand

    data class StartAudio(val message: StartAudioMessage) : HostCommand

    data object StopAudio : HostCommand

    /** Device-to-host types echoed back, or types this version does not know: skipped. */
    data class Ignored(val rawType: Int) : HostCommand
}

object HostCommandDecoder {
    /**
     * @throws MessageDecodingException when the JSON content is invalid.
     * @throws dev.mobilewebcambridge.protocol.wire.ProtocolException for a malformed `Pong`.
     */
    fun decode(packet: Packet): HostCommand {
        val type = packet.type ?: return HostCommand.Ignored(packet.rawType)
        return when (type) {
            PacketType.HELLO -> HostCommand.Hello(HelloMessage.from(MessageCodec.fields(type, packet.payload)))
            PacketType.ERROR -> HostCommand.Error(ErrorMessage.from(MessageCodec.fields(type, packet.payload)))
            PacketType.PING -> HostCommand.Ping(t0 = packet.timestampUs)
            PacketType.PONG -> HostCommand.Pong(PongPayload.decode(packet.payload), sentAtUs = packet.timestampUs)
            PacketType.START_VIDEO -> HostCommand.StartVideo(StartVideoMessage.from(MessageCodec.fields(type, packet.payload)))
            PacketType.STOP_VIDEO -> HostCommand.StopVideo
            PacketType.REQUEST_KEYFRAME -> HostCommand.RequestKeyframe
            PacketType.START_AUDIO -> HostCommand.StartAudio(StartAudioMessage.from(MessageCodec.fields(type, packet.payload)))
            PacketType.STOP_AUDIO -> HostCommand.StopAudio
            PacketType.VIDEO_CONFIG,
            PacketType.VIDEO_ACCESS_UNIT,
            PacketType.AUDIO_CONFIG,
            PacketType.AUDIO_CHUNK,
            PacketType.STATUS,
            PacketType.LOG,
            -> HostCommand.Ignored(packet.rawType)
        }
    }
}

/** Decodes JSON payloads of any type (the device only needs host commands; tests use the rest). */
object MessageCodec {
    fun fields(type: PacketType, payload: ByteArray): JsonFields {
        val json = try {
            JsonParser.parse(payload)
        } catch (error: JsonSyntaxException) {
            throw MessageDecodingException(type, error.message ?: "malformed JSON", MessageDecodingException.Kind.INVALID_JSON)
        }
        return JsonFields.of(json, type)
    }

    /** The typed message carried by a JSON packet type, or null for binary/empty types. */
    fun decode(type: PacketType, payload: ByteArray): JsonMessage? = when (type) {
        PacketType.HELLO -> HelloMessage.from(fields(type, payload))
        PacketType.ERROR -> ErrorMessage.from(fields(type, payload))
        PacketType.START_VIDEO -> StartVideoMessage.from(fields(type, payload))
        PacketType.START_AUDIO -> StartAudioMessage.from(fields(type, payload))
        PacketType.VIDEO_CONFIG -> VideoConfigMessage.from(fields(type, payload))
        PacketType.AUDIO_CONFIG -> AudioConfigMessage.from(fields(type, payload))
        PacketType.STATUS -> StatusMessage.from(fields(type, payload))
        PacketType.LOG -> LogMessage.from(fields(type, payload))
        PacketType.PING,
        PacketType.PONG,
        PacketType.STOP_VIDEO,
        PacketType.REQUEST_KEYFRAME,
        PacketType.STOP_AUDIO,
        PacketType.VIDEO_ACCESS_UNIT,
        PacketType.AUDIO_CHUNK,
        -> null
    }
}
