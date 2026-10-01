package dev.mobilewebcambridge.protocol.wire

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Binary encoding of packets: big-endian header, raw payload (SPEC §1). */
object PacketCodec {
    private const val UINT32_MASK: Long = 0xFFFF_FFFFL

    fun encode(packet: Packet): ByteArray {
        val buffer = ByteBuffer.allocate(PacketHeader.BYTE_COUNT + packet.payload.size).order(ByteOrder.BIG_ENDIAN)
        writeHeader(buffer, packet.header)
        buffer.put(packet.payload)
        return buffer.array()
    }

    fun encodeHeader(header: PacketHeader): ByteArray {
        val buffer = ByteBuffer.allocate(PacketHeader.BYTE_COUNT).order(ByteOrder.BIG_ENDIAN)
        writeHeader(buffer, header)
        return buffer.array()
    }

    /**
     * Decodes the header at [offset] and validates the magic and the payload length rule.
     * Requires at least [PacketHeader.BYTE_COUNT] bytes from [offset].
     */
    fun decodeHeader(bytes: ByteArray, offset: Int = 0): PacketHeader {
        require(bytes.size - offset >= PacketHeader.BYTE_COUNT) {
            "decodeHeader needs ${PacketHeader.BYTE_COUNT} bytes, got ${bytes.size - offset}"
        }
        val buffer = ByteBuffer.wrap(bytes, offset, PacketHeader.BYTE_COUNT).order(ByteOrder.BIG_ENDIAN)
        val magic = buffer.int.toLong() and UINT32_MASK
        val rawType = buffer.get().toInt() and 0xFF
        val flags = buffer.get().toInt() and 0xFF
        buffer.short // reserved: senders write 0, receivers ignore
        val seq = buffer.int.toLong() and UINT32_MASK
        val payloadLength = buffer.int.toLong() and UINT32_MASK
        val timestampUs = buffer.long
        if (magic != PacketHeader.MAGIC) throw ProtocolException.BadMagic(magic)
        ProtocolLimits.rule(rawType).validate(payloadLength, rawType)
        return PacketHeader(rawType, flags, seq, payloadLength, timestampUs)
    }

    private fun writeHeader(buffer: ByteBuffer, header: PacketHeader) {
        buffer.putInt(PacketHeader.MAGIC.toInt())
        buffer.put(header.rawType.toByte())
        buffer.put(header.flags.toByte())
        buffer.putShort(0)
        buffer.putInt(header.seq.toInt())
        buffer.putInt(header.payloadLength.toInt())
        buffer.putLong(header.timestampUs)
    }
}
