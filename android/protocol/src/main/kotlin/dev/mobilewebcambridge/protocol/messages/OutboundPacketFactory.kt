package dev.mobilewebcambridge.protocol.messages

import dev.mobilewebcambridge.protocol.json.CanonicalJson
import dev.mobilewebcambridge.protocol.media.AudioChunk
import dev.mobilewebcambridge.protocol.media.EncodedVideoFrame
import dev.mobilewebcambridge.protocol.wire.AudioFlags
import dev.mobilewebcambridge.protocol.wire.Packet
import dev.mobilewebcambridge.protocol.wire.PacketType
import dev.mobilewebcambridge.protocol.wire.ProtocolException
import dev.mobilewebcambridge.protocol.wire.ProtocolLimits

/**
 * Builds outgoing packets and assigns the per-type `seq` (SPEC §1).
 *
 * Sequence numbers are assigned when a packet is produced, before the send scheduler may drop it,
 * so the host sees every deliberate drop as a gap. Not thread-safe; confine to the session thread.
 */
class OutboundPacketFactory {
    /** Thrown when a payload exceeds the limit of its packet type. */
    class PayloadTooLargeException(val type: PacketType, val length: Int) :
        Exception("${type.name} payload of $length bytes exceeds the protocol limit")

    private val nextSeq = LongArray(256)

    fun packet(type: PacketType, flags: Int = 0, timestampUs: Long, payload: ByteArray = Packet.EMPTY_PAYLOAD): Packet {
        try {
            ProtocolLimits.rule(type.code).validate(payload.size.toLong(), type.code)
        } catch (error: ProtocolException) {
            throw PayloadTooLargeException(type, payload.size).apply { initCause(error) }
        }
        return Packet(type.code, flags, takeSeq(type), timestampUs, payload)
    }

    fun json(type: PacketType, message: JsonMessage, timestampUs: Long): Packet =
        packet(type, timestampUs = timestampUs, payload = CanonicalJson.encode(message.toJson()))

    fun ping(timestampUs: Long): Packet = Packet(PacketType.PING.code, 0, takeSeq(PacketType.PING), timestampUs)

    fun pong(payload: PongPayload, timestampUs: Long): Packet =
        Packet(PacketType.PONG.code, 0, takeSeq(PacketType.PONG), timestampUs, payload.encode())

    fun videoAccessUnit(frame: EncodedVideoFrame): Packet =
        packet(PacketType.VIDEO_ACCESS_UNIT, frame.flags, frame.presentationTimeUs, frame.annexB)

    fun audioChunk(chunk: AudioChunk): Packet =
        packet(
            PacketType.AUDIO_CHUNK,
            if (chunk.isDiscontinuity) AudioFlags.DISCONTINUITY else 0,
            chunk.timestampUs,
            chunk.pcm,
        )

    private fun takeSeq(type: PacketType): Long {
        val seq = nextSeq[type.code]
        nextSeq[type.code] = (seq + 1) and 0xFFFF_FFFFL
        return seq
    }
}
