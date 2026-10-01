package dev.mobilewebcambridge.protocol

import dev.mobilewebcambridge.protocol.logging.LogEntry
import dev.mobilewebcambridge.protocol.logging.LogRing
import dev.mobilewebcambridge.protocol.media.AudioChunk
import dev.mobilewebcambridge.protocol.media.Pcm16
import dev.mobilewebcambridge.protocol.media.ToneGenerator
import dev.mobilewebcambridge.protocol.messages.LogLevel
import dev.mobilewebcambridge.protocol.messages.OutboundPacketFactory
import dev.mobilewebcambridge.protocol.messages.StartAudioMessage
import dev.mobilewebcambridge.protocol.messages.AudioProcessing
import dev.mobilewebcambridge.protocol.wire.PacketCodec
import dev.mobilewebcambridge.protocol.wire.PacketType
import org.junit.jupiter.api.Assertions.assertArrayEquals
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertThrows
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test

class OutboundPacketFactoryTest {
    @Test
    fun `assigns sequence numbers per packet type`() {
        val factory = OutboundPacketFactory()
        assertEquals(0, factory.ping(1).seq)
        assertEquals(1, factory.ping(2).seq)
        assertEquals(0, factory.json(PacketType.START_AUDIO, StartAudioMessage(AudioProcessing.RAW), 3).seq)
        assertEquals(2, factory.ping(4).seq)
    }

    @Test
    fun `encodes the start-audio vector exactly`() {
        val vector = TestVectors.packets.objects("valid").single { it.str("name") == "start-audio" }
        val factory = OutboundPacketFactory()
        val packet = factory.json(PacketType.START_AUDIO, StartAudioMessage(AudioProcessing.STANDARD), timestampUs = 5_001)
        assertEquals(vector.str("hex"), PacketCodec.encode(packet).toHex())
    }

    @Test
    fun `refuses oversized payloads`() {
        val factory = OutboundPacketFactory()
        assertThrows(OutboundPacketFactory.PayloadTooLargeException::class.java) {
            factory.packet(PacketType.AUDIO_CHUNK, timestampUs = 0, payload = ByteArray(65_537))
        }
    }
}

class AudioTest {
    @Test
    fun `splits PCM into chunks of at most 40 ms with only the first discontinuous`() {
        val pcm = ByteArray(2 * 4_000)
        val chunks = AudioChunk.split(pcm, configId = 1, timestampUs = 1_000_000, isDiscontinuity = true)
        assertEquals(listOf(1_920, 1_920, 160), chunks.map { it.sampleCount })
        assertEquals(listOf(true, false, false), chunks.map { it.isDiscontinuity })
        assertEquals(listOf(1_000_000L, 1_040_000L, 1_080_000L), chunks.map { it.timestampUs })
    }

    @Test
    fun `round-trips little-endian samples`() {
        val samples = shortArrayOf(0, 1, -1, Short.MAX_VALUE, Short.MIN_VALUE)
        val bytes = Pcm16.toLittleEndian(samples)
        assertEquals("00000100ffffff7f0080", bytes.toHex())
        assertArrayEquals(samples, Pcm16.fromLittleEndian(bytes))
    }

    @Test
    fun `tone generator stays within its amplitude and is continuous`() {
        val tone = ToneGenerator(frequencyHz = 1_000.0, amplitude = 0.5)
        val samples = tone.next(480) + tone.next(480)
        assertTrue(samples.all { kotlin.math.abs(it.toInt()) <= Short.MAX_VALUE / 2 + 1 })
        for (i in 1 until samples.size) assertTrue(kotlin.math.abs(samples[i] - samples[i - 1]) < 3_000, "jump at $i")
    }
}

class LogRingTest {
    private fun entry(n: Int) = LogEntry(n.toLong(), LogLevel.INFO, "test", "message $n")

    @Test
    fun `drops the oldest entries when full and counts them`() {
        val ring = LogRing(capacity = 3)
        (1..5).forEach { ring.append(entry(it)) }
        assertEquals(3, ring.count)
        assertEquals(2, ring.takeDroppedCount())
        assertEquals(0, ring.takeDroppedCount())
        assertEquals(listOf(3L, 4L), ring.drain(2).map { it.timestampUs })
        assertEquals(listOf(5L), ring.newest(10).map { it.timestampUs })
    }
}
