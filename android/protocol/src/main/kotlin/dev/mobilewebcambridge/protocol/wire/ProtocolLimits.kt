package dev.mobilewebcambridge.protocol.wire

/**
 * Error raised by the packet layer. Every case is fatal: the connection must be closed, because a
 * corrupted stream cannot be resynchronised (SPEC §1.1).
 */
sealed class ProtocolException(message: String) : Exception(message) {
    /** Stable code shared with the host implementation and the golden test vectors. */
    abstract val code: String

    class BadMagic(val magic: Long) : ProtocolException("Bad packet magic 0x${magic.toString(16)}") {
        override val code: String get() = "BAD_MAGIC"
    }

    class PayloadTooLarge(val rawType: Int, val length: Long) :
        ProtocolException("Payload of $length bytes too large for packet type 0x${rawType.toString(16)}") {
        override val code: String get() = "PAYLOAD_TOO_LARGE"
    }

    class BadPayloadLength(val rawType: Int, val length: Long) :
        ProtocolException("Payload length $length invalid for packet type 0x${rawType.toString(16)}") {
        override val code: String get() = "BAD_PAYLOAD_LENGTH"
    }
}

/** Allowed payload length for one packet type. */
sealed class PayloadLengthRule {
    data class Exactly(val length: Long) : PayloadLengthRule()

    data class AtMost(val maximum: Long) : PayloadLengthRule()

    fun validate(length: Long, rawType: Int) {
        when (this) {
            is Exactly -> if (length != this.length) throw ProtocolException.BadPayloadLength(rawType, length)
            is AtMost -> if (length > maximum) throw ProtocolException.PayloadTooLarge(rawType, length)
        }
    }
}

/** Payload limits from SPEC §1.1. */
object ProtocolLimits {
    const val MAX_JSON_PAYLOAD: Long = 65_536
    const val MAX_LOG_PAYLOAD: Long = 8_192
    const val MAX_AUDIO_PAYLOAD: Long = 65_536
    const val MAX_VIDEO_PAYLOAD: Long = 4_194_304
    const val MAX_UNKNOWN_PAYLOAD: Long = 4_194_304
    const val PONG_PAYLOAD_LENGTH: Long = 16

    fun rule(rawType: Int): PayloadLengthRule {
        val type = PacketType.fromCode(rawType) ?: return PayloadLengthRule.AtMost(MAX_UNKNOWN_PAYLOAD)
        return when (type) {
            PacketType.HELLO,
            PacketType.ERROR,
            PacketType.START_VIDEO,
            PacketType.START_AUDIO,
            PacketType.VIDEO_CONFIG,
            PacketType.AUDIO_CONFIG,
            PacketType.STATUS,
            -> PayloadLengthRule.AtMost(MAX_JSON_PAYLOAD)
            PacketType.LOG -> PayloadLengthRule.AtMost(MAX_LOG_PAYLOAD)
            PacketType.PING,
            PacketType.STOP_VIDEO,
            PacketType.REQUEST_KEYFRAME,
            PacketType.STOP_AUDIO,
            -> PayloadLengthRule.Exactly(0)
            PacketType.PONG -> PayloadLengthRule.Exactly(PONG_PAYLOAD_LENGTH)
            PacketType.VIDEO_ACCESS_UNIT -> PayloadLengthRule.AtMost(MAX_VIDEO_PAYLOAD)
            PacketType.AUDIO_CHUNK -> PayloadLengthRule.AtMost(MAX_AUDIO_PAYLOAD)
        }
    }
}
