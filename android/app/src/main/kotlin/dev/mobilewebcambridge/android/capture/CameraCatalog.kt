package dev.mobilewebcambridge.android.capture

import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.os.Build
import android.util.Range
import dev.mobilewebcambridge.protocol.policy.CaptureFormatInfo

/** Reads Camera2 characteristics into plain values for [CameraSelector] and the format selector. */
class CameraCatalog(private val manager: CameraManager) {
    fun describe(): List<CameraDescriptor> = manager.cameraIdList.mapNotNull { id -> runCatching { describe(id) }.getOrNull() }

    fun describe(id: String): CameraDescriptor {
        val characteristics = manager.getCameraCharacteristics(id)
        val facing = when (characteristics.get(CameraCharacteristics.LENS_FACING)) {
            CameraMetadata.LENS_FACING_FRONT -> LensFacing.FRONT
            CameraMetadata.LENS_FACING_BACK -> LensFacing.BACK
            else -> LensFacing.EXTERNAL
        }
        val physical = characteristics.physicalCameraIds.mapNotNull { physicalId ->
            runCatching { PhysicalLens(physicalId, fieldOfView(manager.getCameraCharacteristics(physicalId))) }.getOrNull()
        }
        val zoomRange = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            characteristics.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)?.let { it.lower..it.upper }
        } else {
            null
        }
        return CameraDescriptor(
            id = id,
            facing = facing,
            sensorOrientation = characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0,
            fieldOfView = fieldOfView(characteristics),
            physicalLenses = physical,
            zoomRatioRange = zoomRange,
            realtimeTimestamps = characteristics.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE) ==
                CameraMetadata.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME,
        )
    }

    /** Output sizes a `SurfaceTexture` can receive, with their maximum frame rate. */
    fun surfaceFormats(id: String): List<CaptureFormatInfo> {
        val characteristics = manager.getCameraCharacteristics(id)
        val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return emptyList()
        val sizes = map.getOutputSizes(SurfaceTexture::class.java) ?: return emptyList()
        return sizes.mapIndexed { index, size ->
            val minFrameDurationNs = map.getOutputMinFrameDuration(SurfaceTexture::class.java, size)
            val maxFps = if (minFrameDurationNs > 0) 1e9 / minFrameDurationNs else 30.0
            CaptureFormatInfo(index, size.width, size.height, minFrameRate = 1.0, maxFrameRate = maxFps)
        }
    }

    /**
     * The auto-exposure frame-rate range to request for [fps]: a fixed `[fps, fps]` range when
     * available (steady timing), otherwise the narrowest range containing it.
     */
    fun frameRateRange(id: String, fps: Int): Range<Int>? {
        val ranges = manager.getCameraCharacteristics(id).get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?: return null
        ranges.firstOrNull { it.lower == fps && it.upper == fps }?.let { return it }
        return ranges.filter { it.lower <= fps && it.upper >= fps }.minByOrNull { it.upper - it.lower }
            ?: ranges.maxByOrNull { it.upper }
    }

    private fun fieldOfView(characteristics: CameraCharacteristics): Double {
        val focalLength = characteristics.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.minOrNull()
        val sensorWidth = characteristics.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)?.width
        return if (focalLength != null && focalLength > 0f && sensorWidth != null) sensorWidth / focalLength.toDouble() else 1.0
    }
}
