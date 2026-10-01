package dev.mobilewebcambridge.android.encoding

import android.view.Surface
import dev.mobilewebcambridge.protocol.media.EncodedVideoFrame
import dev.mobilewebcambridge.protocol.messages.EncoderMode

data class EncoderSettings(
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrateKbps: Int,
    val mode: EncoderMode,
)

/** An H.264 encoder fed through its input surface. */
interface VideoEncoder {
    /** The configuration id of the first access units. */
    val configId: Int
    val settings: EncoderSettings
    val inputSurface: Surface

    /** Wire name of the profile actually configured (`VideoConfig.profile`). */
    val profileName: String

    /** The mode actually applied (`VideoConfig.encoder`). */
    val effectiveMode: EncoderMode

    /** Asks for an IDR (with SPS/PPS) as soon as possible. Any thread. */
    fun requestSyncFrame()

    /**
     * Changes the bitrate of the running encoder and starts configuration [configId]: access
     * units carry the new id from the next IDR on, which is requested at once. Video thread.
     */
    fun reconfigure(configId: Int, bitrateKbps: Int)

    fun release()
}

fun interface VideoEncoderFactory {
    /**
     * @param output receives every access unit, on the encoder's thread
     * @param failure receives a description when the encoder dies, on the encoder's thread
     */
    fun create(
        configId: Int,
        settings: EncoderSettings,
        output: (EncodedVideoFrame) -> Unit,
        failure: (String) -> Unit,
    ): VideoEncoder
}

/** H.264 profile choice from what the codec advertises. Pure: unit-tested on the JVM. */
object EncoderProfiles {
    // MediaCodecInfo.CodecProfileLevel values (stable public constants).
    const val BASELINE = 0x01
    const val MAIN = 0x02
    const val HIGH = 0x08
    const val CONSTRAINED_BASELINE = 0x10000
    const val CONSTRAINED_HIGH = 0x80000

    data class Choice(val profile: Int, val level: Int, val wireName: String)

    private val PREFERENCE = listOf(
        CONSTRAINED_HIGH to "constrainedHigh",
        HIGH to "high",
        MAIN to "main",
        CONSTRAINED_BASELINE to "constrainedBaseline",
        BASELINE to "baseline",
    )

    /**
     * Picks the most efficient profile without B-frames the codec supports, with the highest level
     * it advertises for it (always valid for the codec). Null lets the codec choose.
     *
     * @param supported (profile, level) pairs from `CodecCapabilities.profileLevels`
     */
    fun choose(supported: List<Pair<Int, Int>>): Choice? {
        for ((profile, name) in PREFERENCE) {
            val level = supported.filter { it.first == profile }.maxOfOrNull { it.second } ?: continue
            return Choice(profile, level, name)
        }
        return null
    }
}
