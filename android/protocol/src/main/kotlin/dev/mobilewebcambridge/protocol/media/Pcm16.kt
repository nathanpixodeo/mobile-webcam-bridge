package dev.mobilewebcambridge.protocol.media

import kotlin.math.PI
import kotlin.math.roundToInt
import kotlin.math.sin

/** Conversion between 16-bit samples and the wire format (`s16le`). */
object Pcm16 {
    /** Writes the first [count] samples as little-endian signed 16-bit. */
    fun toLittleEndian(samples: ShortArray, count: Int = samples.size): ByteArray {
        val out = ByteArray(count * 2)
        for (i in 0 until count) {
            val value = samples[i].toInt()
            out[2 * i] = (value and 0xFF).toByte()
            out[2 * i + 1] = ((value shr 8) and 0xFF).toByte()
        }
        return out
    }

    fun fromLittleEndian(bytes: ByteArray): ShortArray =
        ShortArray(bytes.size / 2) { i ->
            ((bytes[2 * i].toInt() and 0xFF) or (bytes[2 * i + 1].toInt() shl 8)).toShort()
        }
}

/** Phase-continuous sine generator for the synthetic audio source. */
class ToneGenerator(
    private val frequencyHz: Double = 440.0,
    private val sampleRate: Int = AudioChunk.SAMPLE_RATE,
    private val amplitude: Double = 0.25,
) {
    private var phase = 0.0

    fun next(sampleCount: Int): ShortArray {
        val increment = 2 * PI * frequencyHz / sampleRate
        return ShortArray(sampleCount.coerceAtLeast(0)) {
            val sample = (amplitude * sin(phase) * Short.MAX_VALUE).roundToInt()
            phase += increment
            if (phase >= 2 * PI) phase -= 2 * PI
            sample.coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt()).toShort()
        }
    }
}
