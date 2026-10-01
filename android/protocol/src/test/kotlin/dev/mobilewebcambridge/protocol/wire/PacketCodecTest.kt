package dev.mobilewebcambridge.protocol.wire

import dev.mobilewebcambridge.protocol.SeededRandom
import dev.mobilewebcambridge.protocol.TestVectors
import dev.mobilewebcambridge.protocol.hex
import dev.mobilewebcambridge.protocol.int
import dev.mobilewebcambridge.protocol.messages.PongPayload
import dev.mobilewebcambridge.protocol.objects
import dev.mobilewebcambridge.protocol.obj
import dev.mobilewebcambridge.protocol.str
import dev.mobilewebcambridge.protocol.toHex
import dev.mobilewebcambridge.protocol.long
import org.junit.jupiter.api.Assertions.assertArrayEquals
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertThrows
import org.junit.jupiter.api.DynamicTest
import org.junit.jupiter.api.DynamicTest.dynamicTest
import org.junit.jupiter.api.Test
import org.junit.jupiter.api.TestFactory

class PacketCodecTest {
    private val vectors = TestVectors.packets

    private data class Simple(val type: Int, val flags: Int, val seq: Long, val payloadHex: String)

    private fun simplify(packet: Packet) = Simple(packet.rawType, packet.flags, packet.seq, packet.payload.toHex())

    @TestFactory
    fun `decodes and re-encodes every valid vector`(): List<DynamicTest> =
        vectors.objects("valid").map { vector ->
            dynamicTest(vector.str("name")) {
                val bytes = hex(vector.str("hex"))
                val packets = PacketStreamParser().push(bytes)
                assertEquals(1, packets.size)
                val packet = packets.single()
                val header = vector["header"]!!.obj()
                assertEquals(header.int("type"), packet.rawType)
                assertEquals(header.int("flags"), packet.flags)
                assertEquals(header.long("seq"), packet.seq)
                assertEquals(header.long("payloadLength"), packet.payload.size.toLong())
                assertEquals(header.str("timestampUs").toLong(), packet.timestampUs)

                val payload = vector["payload"]!!.obj()
                when (payload.str("kind")) {
                    "json" -> assertEquals(payload.str("text"), packet.payload.toString(Charsets.UTF_8))
                    "hex" -> assertEquals(payload.str("hex"), packet.payload.toHex())
                    "pong" -> assertEquals(
                        PongPayload(payload.str("echoT0").toLong(), payload.str("t1").toLong()),
                        PongPayload.decode(packet.payload),
                    )
                    "none" -> assertEquals(0, packet.payload.size)
                    else -> error("unknown payload kind")
                }
                assertEquals(vector.str("hex"), PacketCodec.encode(packet).toHex())
            }
        }

    @TestFactory
    fun `rejects every invalid vector with its error code`(): List<DynamicTest> =
        vectors.objects("invalid").map { vector ->
            dynamicTest(vector.str("name")) {
                val error = assertThrows(ProtocolException::class.java) { PacketStreamParser().push(hex(vector.str("hex"))) }
                assertEquals(vector.str("error"), error.code)
            }
        }

    @TestFactory
    fun `parses streams however they are chunked`(): List<DynamicTest> =
        vectors.objects("streams").flatMap { stream ->
            val bytes = hex(stream.str("hex"))
            val expected = stream.objects("expect").map {
                Simple(it.int("type"), it.int("flags"), it.long("seq"), it.str("payloadHex"))
            }
            val name = stream.str("name")
            listOf(
                dynamicTest("$name in one chunk") {
                    assertEquals(expected, PacketStreamParser().push(bytes).map { simplify(it) })
                },
                dynamicTest("$name split at every byte boundary") {
                    for (cut in 1 until bytes.size) {
                        val parser = PacketStreamParser()
                        val packets = parser.push(bytes.copyOfRange(0, cut)) + parser.push(bytes.copyOfRange(cut, bytes.size))
                        assertEquals(expected, packets.map { simplify(it) }, "cut=$cut")
                    }
                },
                dynamicTest("$name one byte at a time") {
                    val parser = PacketStreamParser()
                    val packets = bytes.flatMap { byte -> parser.push(byteArrayOf(byte)) }
                    assertEquals(expected, packets.map { simplify(it) })
                },
                dynamicTest("$name with seeded random chunks") {
                    val random = SeededRandom(0xC0FFEE)
                    repeat(200) { round ->
                        val parser = PacketStreamParser()
                        val packets = ArrayList<Packet>()
                        var offset = 0
                        while (offset < bytes.size) {
                            val size = 1 + (random.next() * 40).toInt()
                            val end = minOf(bytes.size, offset + size)
                            packets += parser.push(bytes, offset, end - offset)
                            offset = end
                        }
                        assertEquals(expected, packets.map { simplify(it) }, "round=$round")
                    }
                },
            )
        }

    @Test
    fun `keeps partial data buffered until the packet completes`() {
        val packet = Packet.of(PacketType.VIDEO_ACCESS_UNIT, seq = 1, timestampUs = 5, payload = ByteArray(1000) { 7 })
        val bytes = PacketCodec.encode(packet)
        val parser = PacketStreamParser()
        assertEquals(emptyList<Packet>(), parser.push(bytes, 0, 500))
        assertEquals(500 - PacketHeader.BYTE_COUNT, parser.bufferedByteCount)
        val decoded = parser.push(bytes, 500, bytes.size - 500).single()
        assertArrayEquals(packet.payload, decoded.payload)
        assertEquals(0, parser.bufferedByteCount)
    }

    @Test
    fun `stays failed after a violation`() {
        val parser = PacketStreamParser()
        assertThrows(ProtocolException::class.java) { parser.push(ByteArray(24) { 0xFF.toByte() }) }
        assertThrows(ProtocolException::class.java) { parser.push(ByteArray(1)) }
    }

    @Test
    fun `grows past its initial buffer for a large access unit`() {
        val payload = ByteArray(300_000) { (it % 251).toByte() }
        val bytes = PacketCodec.encode(Packet.of(PacketType.VIDEO_ACCESS_UNIT, seq = 0, timestampUs = 0, payload = payload))
        val parser = PacketStreamParser()
        val packets = ArrayList<Packet>()
        var offset = 0
        while (offset < bytes.size) {
            val end = minOf(bytes.size, offset + 7_000)
            packets += parser.push(bytes, offset, end - offset)
            offset = end
        }
        assertArrayEquals(payload, packets.single().payload)
    }

    @Test
    fun `round-trips pong payloads including negative timestamps`() {
        val pong = PongPayload(echoT0 = -42, t1 = 9_007_199_254_740_993)
        assertEquals(pong, PongPayload.decode(pong.encode()))
    }
}
