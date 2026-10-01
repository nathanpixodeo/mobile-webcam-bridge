package dev.mobilewebcambridge.protocol.wire

/**
 * Incremental parser: feed it TCP chunks of any size, get back complete packets.
 *
 * The header is validated as soon as its 24 bytes are available, so an oversized length is rejected
 * before any payload is buffered. After the first error the parser stays failed: the connection
 * must be closed (SPEC §1.1). Not thread-safe; confine it to one thread.
 */
class PacketStreamParser {
    private var buffer = ByteArray(INITIAL_CAPACITY)
    private var start = 0
    private var end = 0
    private var pendingHeader: PacketHeader? = null
    private var failure: ProtocolException? = null

    /** Bytes received but not yet returned as part of a packet. */
    val bufferedByteCount: Int get() = end - start

    fun push(chunk: ByteArray, offset: Int = 0, length: Int = chunk.size - offset): List<Packet> {
        failure?.let { throw it }
        append(chunk, offset, length)

        val packets = ArrayList<Packet>()
        try {
            while (true) {
                val header = pendingHeader ?: run {
                    if (end - start < PacketHeader.BYTE_COUNT) return@run null
                    val decoded = PacketCodec.decodeHeader(buffer, start)
                    start += PacketHeader.BYTE_COUNT
                    pendingHeader = decoded
                    decoded
                } ?: break
                val payloadLength = header.payloadLength.toInt()
                if (end - start < payloadLength) break
                val payload = buffer.copyOfRange(start, start + payloadLength)
                start += payloadLength
                pendingHeader = null
                packets += Packet(header.rawType, header.flags, header.seq, header.timestampUs, payload)
            }
        } catch (error: ProtocolException) {
            failure = error
            start = 0
            end = 0
            pendingHeader = null
            throw error
        }
        if (start == end) {
            start = 0
            end = 0
        }
        return packets
    }

    private fun append(chunk: ByteArray, offset: Int, length: Int) {
        if (length <= 0) return
        if (end + length > buffer.size) {
            val live = end - start
            if (live + length <= buffer.size) {
                // Enough room once consumed bytes are dropped: compact in place.
                System.arraycopy(buffer, start, buffer, 0, live)
            } else {
                val grown = ByteArray(maxOf(buffer.size * 2, live + length))
                System.arraycopy(buffer, start, grown, 0, live)
                buffer = grown
            }
            start = 0
            end = live
        }
        System.arraycopy(chunk, offset, buffer, end, length)
        end += length
    }

    private companion object {
        const val INITIAL_CAPACITY = 64 * 1024
    }
}
