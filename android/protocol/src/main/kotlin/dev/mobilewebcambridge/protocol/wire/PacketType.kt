package dev.mobilewebcambridge.protocol.wire

/** Packet type codes, protocol/SPEC.md §3. */
enum class PacketType(val code: Int) {
    HELLO(0x01),
    ERROR(0x02),
    PING(0x03),
    PONG(0x04),
    START_VIDEO(0x10),
    STOP_VIDEO(0x11),
    REQUEST_KEYFRAME(0x12),
    START_AUDIO(0x13),
    STOP_AUDIO(0x14),
    VIDEO_CONFIG(0x20),
    VIDEO_ACCESS_UNIT(0x21),
    AUDIO_CONFIG(0x30),
    AUDIO_CHUNK(0x31),
    STATUS(0x40),
    LOG(0x41),
    ;

    companion object {
        private val byCode: Map<Int, PacketType> = entries.associateBy { it.code }

        /** The known type for [code], or null for a type this version does not know. */
        fun fromCode(code: Int): PacketType? = byCode[code]
    }
}

/** Flags of `VideoAccessUnit` packets. */
object VideoFlags {
    const val IDR: Int = 0x01
    const val PARAMETER_SETS: Int = 0x02
    const val DISPOSABLE: Int = 0x04
}

/** Flags of `AudioChunk` packets. */
object AudioFlags {
    const val DISCONTINUITY: Int = 0x01
}
