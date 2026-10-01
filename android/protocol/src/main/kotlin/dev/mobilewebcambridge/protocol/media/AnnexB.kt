package dev.mobilewebcambridge.protocol.media

import java.io.ByteArrayOutputStream

/**
 * H.264 Annex-B byte-stream helpers (ITU-T H.264 Annex B). MediaCodec already emits Annex-B; these
 * helpers normalise its output to what the wire protocol promises (SPEC §3.3): 4-byte start codes,
 * no access unit delimiter, SPS/PPS in front of every IDR.
 */
object AnnexB {
    val START_CODE: ByteArray = byteArrayOf(0, 0, 0, 1)

    /** Access unit delimiter NAL (`primary_pic_type` = 7), what the host appends after each AU. */
    val ACCESS_UNIT_DELIMITER: ByteArray = byteArrayOf(0, 0, 0, 1, 0x09, 0xF0.toByte())

    object NalType {
        const val NON_IDR_SLICE: Int = 1
        const val IDR_SLICE: Int = 5
        const val SEI: Int = 6
        const val SPS: Int = 7
        const val PPS: Int = 8
        const val ACCESS_UNIT_DELIMITER: Int = 9
    }

    /** `nal_unit_type` of a NAL unit given without its start code. */
    fun nalType(nal: ByteArray): Int = if (nal.isEmpty()) -1 else nal[0].toInt() and 0x1F

    /** `nal_ref_idc`: 0 means no other picture references this one. */
    fun nalRefIdc(nal: ByteArray): Int = if (nal.isEmpty()) 0 else (nal[0].toInt() shr 5) and 0x03

    /**
     * Splits an Annex-B stream into NAL units (without start codes). Accepts 3- and 4-byte start
     * codes and strips `trailing_zero_8bits`, which never belong to a NAL unit.
     */
    fun splitNalUnits(stream: ByteArray): List<ByteArray> {
        val units = ArrayList<ByteArray>()
        var unitStart = -1
        var index = 0
        while (index + 2 < stream.size) {
            if (stream[index].toInt() == 0 && stream[index + 1].toInt() == 0 && stream[index + 2].toInt() == 1) {
                if (unitStart >= 0) addTrimmed(stream, unitStart, index, units)
                index += 3
                unitStart = index
            } else {
                index++
            }
        }
        if (unitStart >= 0) addTrimmed(stream, unitStart, stream.size, units)
        return units
    }

    private fun addTrimmed(stream: ByteArray, start: Int, end: Int, out: MutableList<ByteArray>) {
        var trimmedEnd = end
        while (trimmedEnd > start && stream[trimmedEnd - 1].toInt() == 0) trimmedEnd--
        if (trimmedEnd > start) out += stream.copyOfRange(start, trimmedEnd)
    }

    fun containsIdr(stream: ByteArray): Boolean = splitNalUnits(stream).any { nalType(it) == NalType.IDR_SLICE }

    fun containsParameterSets(stream: ByteArray): Boolean {
        val types = splitNalUnits(stream).map { nalType(it) }.toSet()
        return NalType.SPS in types && NalType.PPS in types
    }

    /**
     * True when the access unit contains coded slices and none of them is referenced by later
     * pictures, so dropping it cannot corrupt decoding.
     */
    fun isDisposable(stream: ByteArray): Boolean {
        val slices = splitNalUnits(stream).filter { nalType(it) == NalType.NON_IDR_SLICE || nalType(it) == NalType.IDR_SLICE }
        return slices.isNotEmpty() && slices.all { nalRefIdc(it) == 0 }
    }

    /** Joins NAL units with 4-byte start codes. */
    fun join(nals: List<ByteArray>): ByteArray {
        val out = ByteArrayOutputStream(nals.sumOf { it.size + START_CODE.size })
        for (nal in nals) {
            out.write(START_CODE)
            out.write(nal)
        }
        return out.toByteArray()
    }

    /**
     * Rewrites an encoder access unit into the wire form: 4-byte start codes, access unit
     * delimiters removed, and [parameterSets] (Annex-B SPS/PPS) prepended to an IDR that lacks them.
     */
    fun normalizeAccessUnit(accessUnit: ByteArray, parameterSets: ByteArray?): ByteArray {
        val nals = splitNalUnits(accessUnit).filter { nalType(it) != NalType.ACCESS_UNIT_DELIMITER }
        val types = nals.map { nalType(it) }.toSet()
        val needsParameterSets = NalType.IDR_SLICE in types && !(NalType.SPS in types && NalType.PPS in types)
        val prefix = if (needsParameterSets && parameterSets != null) {
            splitNalUnits(parameterSets).filter { nalType(it) == NalType.SPS || nalType(it) == NalType.PPS }
        } else {
            emptyList()
        }
        return join(prefix + nals)
    }
}
