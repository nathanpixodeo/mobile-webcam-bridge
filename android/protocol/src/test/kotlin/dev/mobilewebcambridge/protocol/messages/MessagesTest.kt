package dev.mobilewebcambridge.protocol.messages

import dev.mobilewebcambridge.protocol.TestVectors
import dev.mobilewebcambridge.protocol.int
import dev.mobilewebcambridge.protocol.json.CanonicalJson
import dev.mobilewebcambridge.protocol.json.JsonParser
import dev.mobilewebcambridge.protocol.obj
import dev.mobilewebcambridge.protocol.objects
import dev.mobilewebcambridge.protocol.str
import dev.mobilewebcambridge.protocol.wire.Packet
import dev.mobilewebcambridge.protocol.wire.PacketType
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertNotNull
import org.junit.jupiter.api.Assertions.assertThrows
import org.junit.jupiter.api.DynamicTest
import org.junit.jupiter.api.DynamicTest.dynamicTest
import org.junit.jupiter.api.Test
import org.junit.jupiter.api.TestFactory

class MessagesTest {
    @TestFactory
    fun `every JSON vector decodes and re-encodes byte for byte`(): List<DynamicTest> =
        TestVectors.packets.objects("valid")
            .filter { it["payload"]!!.obj().str("kind") == "json" }
            .map { vector ->
                dynamicTest(vector.str("name")) {
                    val text = vector["payload"]!!.obj().str("text")
                    val type = PacketType.fromCode(vector["header"]!!.obj().int("type"))!!
                    val message = MessageCodec.decode(type, text.toByteArray(Charsets.UTF_8))
                    assertNotNull(message)
                    assertEquals(text, CanonicalJson.write(message!!.toJson()))
                    // The vector's JSON value itself is canonical.
                    assertEquals(text, CanonicalJson.write(JsonParser.parse(text)))
                }
            }

    @Test
    fun `decodes host commands`() {
        val start = """{"bitrateKbps":6000,"camera":"front","encoder":"standard","fps":30,"height":1080,"mirror":true,"orientation":"portrait","width":1920}"""
        val command = HostCommandDecoder.decode(Packet.of(PacketType.START_VIDEO, seq = 0, timestampUs = 0, payload = start.toByteArray()))
        assertEquals(
            HostCommand.StartVideo(
                StartVideoMessage(1920, 1080, 30, 6000, CameraSelection.FRONT, true, OrientationMode.PORTRAIT, EncoderMode.STANDARD),
            ),
            command,
        )
        assertEquals(HostCommand.Ping(77), HostCommandDecoder.decode(Packet.of(PacketType.PING, seq = 0, timestampUs = 77)))
        assertEquals(HostCommand.Ignored(0x7F), HostCommandDecoder.decode(Packet(0x7F, 0, 0, 0, byteArrayOf(1, 2, 3))))
        assertEquals(HostCommand.Ignored(PacketType.STATUS.code), HostCommandDecoder.decode(Packet.of(PacketType.STATUS, seq = 0, timestampUs = 0, payload = "{}".toByteArray())))
    }

    @Test
    fun `ignores unknown keys (forward compatibility)`() {
        val payload = """{"futureField":{"x":[1,2.5]},"processing":"raw"}""".toByteArray()
        val command = HostCommandDecoder.decode(Packet.of(PacketType.START_AUDIO, seq = 0, timestampUs = 0, payload = payload))
        assertEquals(HostCommand.StartAudio(StartAudioMessage(AudioProcessing.RAW)), command)
    }

    @Test
    fun `reports malformed JSON as INVALID_JSON and bad values as INVALID_VALUE`() {
        val malformed = assertThrows(MessageDecodingException::class.java) {
            HostCommandDecoder.decode(Packet.of(PacketType.START_AUDIO, seq = 0, timestampUs = 0, payload = "{nope".toByteArray()))
        }
        assertEquals(MessageDecodingException.Kind.INVALID_JSON, malformed.kind)

        val unsupported = assertThrows(MessageDecodingException::class.java) {
            HostCommandDecoder.decode(Packet.of(PacketType.START_AUDIO, seq = 0, timestampUs = 0, payload = """{"processing":"loud"}""".toByteArray()))
        }
        assertEquals(MessageDecodingException.Kind.INVALID_VALUE, unsupported.kind)

        val outOfRange = """{"bitrateKbps":6000,"camera":"front","encoder":"standard","fps":240,"height":720,"mirror":false,"orientation":"auto","width":1280}"""
        val range = assertThrows(MessageDecodingException::class.java) {
            HostCommandDecoder.decode(Packet.of(PacketType.START_VIDEO, seq = 0, timestampUs = 0, payload = outOfRange.toByteArray()))
        }
        assertEquals(MessageDecodingException.Kind.INVALID_VALUE, range.kind)
    }

    @Test
    fun `accepts bitrates up to 40000 and rejects above`() {
        fun decode(bitrate: Int) = HostCommandDecoder.decode(
            Packet.of(
                PacketType.START_VIDEO, seq = 0, timestampUs = 0,
                payload = """{"bitrateKbps":$bitrate,"camera":"back.wide","encoder":"lowLatency","fps":30,"height":2160,"mirror":false,"orientation":"auto","width":3840}""".toByteArray(),
            ),
        )
        assertEquals(40_000, (decode(40_000) as HostCommand.StartVideo).message.bitrateKbps)
        val error = assertThrows(MessageDecodingException::class.java) { decode(40_001) }
        assertEquals(MessageDecodingException.Kind.INVALID_VALUE, error.kind)
    }

    @Test
    fun `truncates long log messages on a code point boundary`() {
        val message = LogMessage(LogLevel.INFO, "test", "é".repeat(5_000))
        val truncated = message.truncated(maxMessageBytes = 101)
        assertEquals("é".repeat(50) + "…", truncated.message)
    }

    @Test
    fun `hello sorts its features on the wire`() {
        val hello = HelloMessage(
            role = PeerRole.DEVICE,
            app = AppDescriptor("app", "1", "2", "sha"),
            features = listOf("video.h264", "audio.pcm"),
        )
        assertEquals(
            """{"app":{"build":"2","gitSha":"sha","name":"app","version":"1"},"features":["audio.pcm","video.h264"],"protocol":{"major":1,"minor":0},"role":"device"}""",
            CanonicalJson.write(hello.toJson()),
        )
    }
}
