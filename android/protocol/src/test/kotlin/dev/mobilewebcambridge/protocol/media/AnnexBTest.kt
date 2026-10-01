package dev.mobilewebcambridge.protocol.media

import dev.mobilewebcambridge.protocol.TestVectors
import dev.mobilewebcambridge.protocol.bool
import dev.mobilewebcambridge.protocol.hex
import dev.mobilewebcambridge.protocol.int
import dev.mobilewebcambridge.protocol.objects
import dev.mobilewebcambridge.protocol.str
import dev.mobilewebcambridge.protocol.toHex
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.DynamicTest
import org.junit.jupiter.api.DynamicTest.dynamicTest
import org.junit.jupiter.api.Test
import org.junit.jupiter.api.TestFactory

class AnnexBTest {
    private val vectors = TestVectors.h264

    @Test
    fun `access unit delimiter matches the vectors`() {
        assertEquals(vectors.str("accessUnitDelimiter"), AnnexB.ACCESS_UNIT_DELIMITER.toHex())
    }

    @TestFactory
    fun `splits the golden streams`(): List<DynamicTest> =
        vectors.objects("split").map { vector ->
            dynamicTest(vector.str("name")) {
                val stream = hex(vector.str("annexB"))
                val nals = AnnexB.splitNalUnits(stream)
                assertEquals(
                    vector.objects("nals").map { it.int("type") to it.str("hex") },
                    nals.map { AnnexB.nalType(it) to it.toHex() },
                )
                assertEquals(vector.bool("isIdr"), AnnexB.containsIdr(stream))
                assertEquals(vector.bool("hasParameterSets"), AnnexB.containsParameterSets(stream))
            }
        }

    @Test
    fun `prepends parameter sets to an IDR that lacks them`() {
        val vector = vectors.objects("avccToAnnexB").single { it.str("name") == "idr-with-prepended-parameter-sets" }
        val parameterSets = hex("00000001" + vector.str("sps") + "00000001" + vector.str("pps"))
        val idrOnly = hex("000001" + "65888400" + "33ff")
        assertEquals(vector.str("annexB"), AnnexB.normalizeAccessUnit(idrOnly, parameterSets).toHex())
    }

    @Test
    fun `normalises start codes and drops access unit delimiters`() {
        val encoderOutput = hex("00000109f0" + "000001419a0203" + "0000")
        assertEquals("00000001419a0203", AnnexB.normalizeAccessUnit(encoderOutput, null).toHex())
    }

    @Test
    fun `detects disposable (non-reference) access units`() {
        // nal_ref_idc 0 (0x01) is disposable; nal_ref_idc 2 (0x41) is a reference frame.
        assertTrue(AnnexB.isDisposable(hex("0000000101aabb")))
        assertFalse(AnnexB.isDisposable(hex("00000001419a02")))
        assertFalse(AnnexB.isDisposable(hex("000000010605ff")))
    }

    @Test
    fun `derives frame flags from the NAL units`() {
        val idr = hex("00000001" + "6742c01f8c8d40" + "00000001" + "68ce3c80" + "00000001" + "6588840033ff")
        val frame = EncodedVideoFrame.fromAnnexB(configId = 1, annexB = idr, encoderKeyframe = false, presentationTimeUs = 9)
        assertEquals(0x03, frame.flags)
        val disposable = EncodedVideoFrame.fromAnnexB(1, hex("0000000101aabb"), encoderKeyframe = false, presentationTimeUs = 9)
        assertEquals(0x04, disposable.flags)
    }
}
