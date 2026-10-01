package dev.mobilewebcambridge.android.capture

import dev.mobilewebcambridge.protocol.messages.CameraSelection

enum class LensFacing { BACK, FRONT, EXTERNAL }

data class PhysicalLens(val id: String, val fieldOfView: Double)

/** Camera2 facts reduced to plain values, so camera choice is testable without a device. */
data class CameraDescriptor(
    val id: String,
    val facing: LensFacing,
    val sensorOrientation: Int,
    /** Sensor width over focal length: bigger means a wider field of view. */
    val fieldOfView: Double,
    val physicalLenses: List<PhysicalLens> = emptyList(),
    /** `CONTROL_ZOOM_RATIO_RANGE` (API 30+); a lower bound below 1 means an ultra-wide is reachable. */
    val zoomRatioRange: ClosedFloatingPointRange<Float>? = null,
    /** Sensor timestamps use `elapsedRealtime` (otherwise they use the monotonic clock). */
    val realtimeTimestamps: Boolean = true,
)

/** The camera to open, an optional zoom ratio to select a lens of a logical camera, and what to report. */
data class CameraChoice(val camera: CameraDescriptor, val zoomRatio: Float?, val actual: CameraSelection)

/**
 * Maps the protocol's camera names to this device's cameras (SPEC §4 `StartVideo.camera`).
 *
 * Ultra-wide and telephoto come from a separate back camera when the device exposes one, otherwise
 * from the zoom ratio of the logical back camera. A device without the lens falls back to
 * `back.wide` and reports that in `VideoConfig.camera`; it never fakes a lens with digital zoom.
 */
class CameraSelector(
    private val ultraWideRatio: Double = 1.25,
    private val telephotoRatio: Double = 0.75,
) {
    fun choose(selection: CameraSelection, cameras: List<CameraDescriptor>): CameraChoice? {
        val main = cameras.firstOrNull { it.facing == LensFacing.BACK }
        val front = cameras.firstOrNull { it.facing == LensFacing.FRONT }
        val fallback = main?.let { CameraChoice(it, null, CameraSelection.BACK_WIDE) }
            ?: front?.let { CameraChoice(it, null, CameraSelection.FRONT) }
            ?: cameras.firstOrNull()?.let { CameraChoice(it, null, CameraSelection.BACK_WIDE) }
        return when (selection) {
            CameraSelection.FRONT -> front?.let { CameraChoice(it, null, CameraSelection.FRONT) } ?: fallback
            CameraSelection.BACK_WIDE -> fallback
            CameraSelection.BACK_ULTRA_WIDE -> main?.let { ultraWide(it, cameras) } ?: fallback
            CameraSelection.BACK_TELEPHOTO -> main?.let { telephoto(it, cameras) } ?: fallback
        }
    }

    private fun ultraWide(main: CameraDescriptor, cameras: List<CameraDescriptor>): CameraChoice? {
        cameras
            .filter { it.facing == LensFacing.BACK && it.id != main.id && it.fieldOfView > main.fieldOfView * ultraWideRatio }
            .maxByOrNull { it.fieldOfView }
            ?.let { return CameraChoice(it, null, CameraSelection.BACK_ULTRA_WIDE) }
        val range = main.zoomRatioRange
        if (range != null && range.start < 0.95f) return CameraChoice(main, range.start, CameraSelection.BACK_ULTRA_WIDE)
        return null
    }

    private fun telephoto(main: CameraDescriptor, cameras: List<CameraDescriptor>): CameraChoice? {
        cameras
            .filter { it.facing == LensFacing.BACK && it.id != main.id && it.fieldOfView < main.fieldOfView * telephotoRatio }
            .minByOrNull { it.fieldOfView }
            ?.let { return CameraChoice(it, null, CameraSelection.BACK_TELEPHOTO) }
        val tele = main.physicalLenses.filter { it.fieldOfView < main.fieldOfView * telephotoRatio }.minByOrNull { it.fieldOfView }
        val range = main.zoomRatioRange
        if (tele != null && range != null) {
            val factor = (main.fieldOfView / tele.fieldOfView).toFloat().coerceIn(1f, range.endInclusive)
            if (factor > 1.05f) return CameraChoice(main, factor, CameraSelection.BACK_TELEPHOTO)
        }
        return null
    }
}
