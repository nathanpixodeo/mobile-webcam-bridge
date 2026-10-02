package dev.mobilewebcambridge.protocol.policy

/** A video size (landscape terms) and frame rate the encoder is asked to produce. */
data class EncodeTarget(val width: Int, val height: Int, val fps: Int)

/**
 * Fits a requested size and frame rate to what the hardware encoder can do (SPEC §3.3: the device
 * encodes the closest size it can and reports it in `VideoConfig`).
 *
 * The size shrinks in fixed steps that turn 3840x2160 into 2560x1440, 1920x1080, 1280x720,
 * 960x540 and 640x360, and keep the aspect ratio of any other request. When no size works at the
 * requested rate, the size is kept and the frame rate drops instead.
 */
object EncodeSizePlanner {
    private const val MIN_WIDTH = 160
    private const val MIN_HEIGHT = 120

    // Scale factors as numerator/denominator, largest first.
    private val SCALES = listOf(1 to 1, 2 to 3, 1 to 2, 1 to 3, 1 to 4, 1 to 6)
    private val LOWER_RATES = listOf(30, 24, 15)

    /**
     * @param supports whether the encoder handles (width, height, fps)
     * @param widthAlignment encoder width granularity (H.264 is at least 2)
     * @param heightAlignment encoder height granularity
     * @return the first supported target, or [requested] unchanged when nothing is supported so
     *   the encoder reports its own failure
     */
    fun plan(
        requested: EncodeTarget,
        widthAlignment: Int = 2,
        heightAlignment: Int = 2,
        supports: (width: Int, height: Int, fps: Int) -> Boolean,
    ): EncodeTarget {
        for ((numerator, denominator) in SCALES) {
            val width = align(requested.width * numerator / denominator, widthAlignment)
            val height = align(requested.height * numerator / denominator, heightAlignment)
            if (width < MIN_WIDTH || height < MIN_HEIGHT) break
            if (supports(width, height, requested.fps)) return EncodeTarget(width, height, requested.fps)
        }
        for (fps in LOWER_RATES.filter { it < requested.fps }) {
            if (supports(requested.width, requested.height, fps)) return requested.copy(fps = fps)
        }
        return requested
    }

    private fun align(value: Int, alignment: Int): Int = value - value % alignment.coerceAtLeast(1)
}
