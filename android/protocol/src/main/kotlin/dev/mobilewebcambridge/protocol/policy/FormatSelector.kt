package dev.mobilewebcambridge.protocol.policy

/**
 * A capture format described with plain values, so selection logic is testable without Camera2.
 * Dimensions are in the sensor's native (landscape) orientation.
 */
data class CaptureFormatInfo(
    val index: Int,
    val width: Int,
    val height: Int,
    val minFrameRate: Double,
    val maxFrameRate: Double,
) {
    internal val area: Long get() = width.toLong() * height

    internal fun supports(fps: Int): Boolean = minFrameRate <= fps + 0.01 && maxFrameRate + 0.01 >= fps
}

/**
 * Chooses the capture format for a requested size and frame rate (same rules as BridgeKit).
 *
 * Preference order: exact size; then the smallest larger format with the same aspect ratio; then
 * the smallest format covering the requested size; then the largest available. Among equals, the
 * lowest maximum frame rate wins (it is usually the lower-power mode).
 */
class FormatSelector {
    fun select(formats: List<CaptureFormatInfo>, width: Int, height: Int, fps: Int): CaptureFormatInfo? {
        val fpsCapable = formats.filter { it.supports(fps) }
        val candidates = fpsCapable.ifEmpty { formats }
        if (candidates.isEmpty()) return null

        candidates.filter { it.width == width && it.height == height }
            .minWithOrNull(compareBy<CaptureFormatInfo> { it.maxFrameRate }.thenBy { it.index })
            ?.let { return it }

        candidates.filter { it.width.toLong() * height == it.height.toLong() * width && it.width >= width && it.height >= height }
            .minWithOrNull(smallest)
            ?.let { return it }

        candidates.filter { it.width >= width && it.height >= height }
            .minWithOrNull(smallest)
            ?.let { return it }

        return candidates.maxWithOrNull(compareBy<CaptureFormatInfo> { it.area }.thenByDescending { it.maxFrameRate })
    }

    private val smallest: Comparator<CaptureFormatInfo> =
        compareBy<CaptureFormatInfo> { it.area }.thenBy { it.maxFrameRate }.thenBy { it.index }
}
