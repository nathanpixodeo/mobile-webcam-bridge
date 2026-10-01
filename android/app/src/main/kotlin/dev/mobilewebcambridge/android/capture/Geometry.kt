package dev.mobilewebcambridge.android.capture

import dev.mobilewebcambridge.protocol.messages.OrientationMode
import kotlin.math.abs
import kotlin.math.roundToInt

/** Orientation arithmetic shared by the camera and synthetic sources. Pure: unit-tested on the JVM. */
object RotationMath {
    /**
     * Quantises an `OrientationEventListener` angle (degrees clockwise from the natural
     * orientation) to 0/90/180/270, keeping [previous] until the angle is [hysteresis] degrees past
     * the boundary, so a phone held near 45° does not flip back and forth.
     */
    fun quantize(angle: Int, previous: Int, hysteresis: Int = 20): Int {
        val normalized = ((angle % 360) + 360) % 360
        val distance = abs(((normalized - previous + 540) % 360) - 180)
        if (distance < 45 + hysteresis) return previous
        return ((normalized + 45) / 90 % 4) * 90
    }

    /**
     * Clockwise rotation that makes the raw sensor image upright (the Camera2 JPEG orientation
     * formula): back cameras add the device rotation, front cameras subtract it.
     */
    fun imageRotation(sensorOrientation: Int, deviceOrientation: Int, frontFacing: Boolean): Int =
        if (frontFacing) {
            ((sensorOrientation - deviceOrientation) % 360 + 360) % 360
        } else {
            (sensorOrientation + deviceOrientation) % 360
        }

    /** Whether the encoded frames are landscape for the requested mode and current device pose. */
    fun isLandscapeOutput(mode: OrientationMode, deviceOrientation: Int): Boolean = when (mode) {
        OrientationMode.LANDSCAPE -> true
        OrientationMode.PORTRAIT -> false
        OrientationMode.AUTO -> deviceOrientation == 90 || deviceOrientation == 270
    }
}

/**
 * Texture coordinates for drawing a camera buffer into the encoder frame: rotate the sensor image
 * by the requested rotation, centre-crop it to the output aspect ratio, optionally mirror it.
 *
 * The `SurfaceTexture` transform matrix may already contain part of that rotation (Camera2 sets a
 * buffer transform from the sensor orientation on many devices, plus a horizontal flip for front
 * cameras). Rather than guessing, the transform is read back from the matrix and compensated, so
 * the result is the same whichever way the producer delivers its buffers.
 */
object TextureMapping {
    private val OUTPUT_CORNERS = arrayOf(
        floatArrayOf(0f, 0f), // bottom-left
        floatArrayOf(1f, 0f), // bottom-right
        floatArrayOf(0f, 1f), // top-left
        floatArrayOf(1f, 1f), // top-right
    )

    /**
     * Returns 8 floats: (s, t) for the four quad corners in triangle-strip order bottom-left,
     * bottom-right, top-left, top-right (GL convention, origin bottom-left).
     *
     * @param stMatrix column-major 4×4 matrix from `SurfaceTexture.getTransformMatrix`
     * @param rotation clockwise rotation (0/90/180/270) to apply to the raw sensor image
     * @param sourceWidth buffer width in sensor orientation
     * @param sourceHeight buffer height in sensor orientation
     */
    fun cornerTexCoords(
        stMatrix: FloatArray,
        rotation: Int,
        mirror: Boolean,
        sourceWidth: Int,
        sourceHeight: Int,
        outputWidth: Int,
        outputHeight: Int,
    ): FloatArray {
        require(stMatrix.size == 16) { "stMatrix must be 4x4" }
        val quarterTurns = ((rotation / 90) % 4 + 4) % 4
        val rotatedWidth = if (quarterTurns % 2 == 1) sourceHeight else sourceWidth
        val rotatedHeight = if (quarterTurns % 2 == 1) sourceWidth else sourceHeight
        val contentAspect = rotatedWidth.toFloat() / rotatedHeight
        val outputAspect = outputWidth.toFloat() / outputHeight
        val cropX = if (contentAspect > outputAspect) outputAspect / contentAspect else 1f
        val cropY = if (contentAspect > outputAspect) 1f else contentAspect / outputAspect
        val producer = producerTransform(stMatrix)

        val result = FloatArray(8)
        OUTPUT_CORNERS.forEachIndexed { index, corner ->
            // Output corner in centred coordinates, undo the mirror and the crop.
            var x = corner[0] - 0.5f
            var y = corner[1] - 0.5f
            if (mirror) x = -x
            x *= cropX
            y *= cropY
            // Undo the clockwise rotation: content → raw sensor coordinates.
            repeat(quarterTurns) {
                val turnedX = -y
                y = x
                x = turnedX
            }
            // Raw sensor → what the SurfaceTexture presents (its producer transform).
            val displayX = producer[0] * x + producer[1] * y
            val displayY = producer[2] * x + producer[3] * y
            val s = displayX + 0.5f
            val t = displayY + 0.5f
            result[2 * index] = stMatrix[0] * s + stMatrix[4] * t + stMatrix[12]
            result[2 * index + 1] = stMatrix[1] * s + stMatrix[5] * t + stMatrix[13]
        }
        return result
    }

    /**
     * The producer transform contained in [stMatrix], as a row-major 2×2 integer matrix mapping raw
     * sensor coordinates (x right, y up) to the coordinates the SurfaceTexture presents. A matrix
     * without producer transform is the plain vertical flip `diag(1, -1)`.
     */
    internal fun producerTransform(stMatrix: FloatArray): IntArray {
        val linear = floatArrayOf(stMatrix[0], stMatrix[4], stMatrix[1], stMatrix[5])
        val scale = linear.maxOf { abs(it) }
        if (scale == 0f) return intArrayOf(1, 0, 0, 1)
        val l = IntArray(4) { (linear[it] / scale).roundToInt() }
        // stMatrix = FlipY · producer⁻¹  ⇒  producer⁻¹ = FlipY · stMatrix (FlipY is its own inverse).
        val inverse = intArrayOf(l[0], l[1], -l[2], -l[3])
        val determinant = inverse[0] * inverse[3] - inverse[1] * inverse[2]
        if (abs(determinant) != 1 || inverse.count { it != 0 } != 2) return intArrayOf(1, 0, 0, 1)
        // Rotations and flips are orthogonal: the inverse is the transpose.
        return intArrayOf(inverse[0], inverse[2], inverse[1], inverse[3])
    }
}
