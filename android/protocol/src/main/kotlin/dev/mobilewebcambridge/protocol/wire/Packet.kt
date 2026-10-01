package dev.mobilewebcambridge.protocol.wire

/** The fixed 24-byte packet header (SPEC §1). `seq` and `payloadLength` are unsigned 32-bit. */
data class PacketHeader(
    val rawType: Int,
    val flags: Int,
    val seq: Long,
    val payloadLength: Long,
    val timestampUs: Long,
) {
    val type: PacketType? get() = PacketType.fromCode(rawType)

    companion object {
        const val BYTE_COUNT: Int = 24

        /** ASCII "MWBR". */
        const val MAGIC: Long = 0x4D574252L
    }
}

/** One packet: header fields plus payload. The payload length is derived from [payload]. */
class Packet(
    val rawType: Int,
    val flags: Int = 0,
    val seq: Long,
    val timestampUs: Long,
    val payload: ByteArray = EMPTY_PAYLOAD,
) {
    val type: PacketType? get() = PacketType.fromCode(rawType)

    val header: PacketHeader
        get() = PacketHeader(rawType, flags, seq, payload.size.toLong(), timestampUs)

    override fun equals(other: Any?): Boolean =
        other is Packet &&
            other.rawType == rawType &&
            other.flags == flags &&
            other.seq == seq &&
            other.timestampUs == timestampUs &&
            other.payload.contentEquals(payload)

    override fun hashCode(): Int {
        var result = rawType
        result = 31 * result + flags
        result = 31 * result + seq.hashCode()
        result = 31 * result + timestampUs.hashCode()
        result = 31 * result + payload.contentHashCode()
        return result
    }

    override fun toString(): String =
        "Packet(type=0x${rawType.toString(16)}, flags=$flags, seq=$seq, timestampUs=$timestampUs, payload=${payload.size} B)"

    companion object {
        val EMPTY_PAYLOAD: ByteArray = ByteArray(0)

        fun of(
            type: PacketType,
            flags: Int = 0,
            seq: Long,
            timestampUs: Long,
            payload: ByteArray = EMPTY_PAYLOAD,
        ): Packet = Packet(type.code, flags, seq, timestampUs, payload)
    }
}
