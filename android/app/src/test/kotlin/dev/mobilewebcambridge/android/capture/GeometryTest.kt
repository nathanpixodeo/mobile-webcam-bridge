package dev.mobilewebcambridge.android.capture

import dev.mobilewebcambridge.protocol.messages.CameraSelection
import dev.mobilewebcambridge.protocol.messages.OrientationMode
import org.junit.jupiter.api.Assertions.assertArrayEquals
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertNull
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test

class RotationMathTest {
    @Test
    fun `quantizes with hysteresis`() {
        assertEquals(0, RotationMath.quantize(50, previous = 0))
        assertEquals(90, RotationMath.quantize(70, previous = 0))
        assertEquals(0, RotationMath.quantize(300, previous = 0))
        assertEquals(270, RotationMath.quantize(290, previous = 0))
        assertEquals(90, RotationMath.quantize(40, previous = 90))
        assertEquals(0, RotationMath.quantize(20, previous = 90))
        assertEquals(0, RotationMath.quantize(359, previous = 180))
    }

    @Test
    fun `matches the Camera2 JPEG orientation formula`() {
        // Typical back camera (sensor 90): portrait needs 90°, landscape with the right side up needs none.
        assertEquals(90, RotationMath.imageRotation(sensorOrientation = 90, deviceOrientation = 0, frontFacing = false))
        assertEquals(0, RotationMath.imageRotation(90, 270, frontFacing = false))
        assertEquals(180, RotationMath.imageRotation(90, 90, frontFacing = false))
        // Typical front camera (sensor 270).
        assertEquals(270, RotationMath.imageRotation(270, 0, frontFacing = true))
        assertEquals(180, RotationMath.imageRotation(270, 90, frontFacing = true))
        assertEquals(0, RotationMath.imageRotation(270, 270, frontFacing = true))
    }

    @Test
    fun `chooses the output orientation`() {
        assertTrue(RotationMath.isLandscapeOutput(OrientationMode.LANDSCAPE, 0))
        assertFalse(RotationMath.isLandscapeOutput(OrientationMode.PORTRAIT, 90))
        assertTrue(RotationMath.isLandscapeOutput(OrientationMode.AUTO, 270))
        assertFalse(RotationMath.isLandscapeOutput(OrientationMode.AUTO, 180))
    }
}

class TextureMappingTest {
    /** SurfaceTexture matrix of a buffer without producer transform: a vertical flip. */
    private val flipY = floatArrayOf(1f, 0f, 0f, 0f, 0f, -1f, 0f, 0f, 0f, 0f, 1f, 0f, 0f, 1f, 0f, 1f)

    /** Producer rotated the buffer 90° clockwise (as Camera2 does for a sensor oriented at 90°). */
    private val producerRotated90 = floatArrayOf(0f, -1f, 0f, 0f, -1f, 0f, 0f, 0f, 0f, 0f, 1f, 0f, 1f, 1f, 0f, 1f)

    /** Producer mirrored the buffer horizontally (front camera preview). */
    private val producerMirrored = floatArrayOf(-1f, 0f, 0f, 0f, 0f, -1f, 0f, 0f, 0f, 0f, 1f, 0f, 1f, 1f, 0f, 1f)

    private fun coords(matrix: FloatArray, rotation: Int, mirror: Boolean = false, source: Pair<Int, Int> = 1920 to 1080, output: Pair<Int, Int>) =
        TextureMapping.cornerTexCoords(matrix, rotation, mirror, source.first, source.second, output.first, output.second)

    @Test
    fun `identity rotation samples the buffer corners`() {
        assertArrayEquals(floatArrayOf(0f, 1f, 1f, 1f, 0f, 0f, 1f, 0f), coords(flipY, 0, output = 1920 to 1080), 1e-6f)
    }

    @Test
    fun `quarter turn maps the output bottom-left to the sensor bottom-right`() {
        val result = coords(flipY, 90, output = 1080 to 1920)
        assertEquals(1f, result[0], 1e-6f)
        assertEquals(1f, result[1], 1e-6f)
    }

    @Test
    fun `compensates a producer rotation so the same sensor pixels are sampled`() {
        for (rotation in listOf(0, 90, 180, 270)) {
            val output = if (rotation % 180 == 0) 1280 to 720 else 720 to 1280
            assertArrayEquals(coords(flipY, rotation, output = output), coords(producerRotated90, rotation, output = output), 1e-5f, "rotation $rotation")
            assertArrayEquals(coords(flipY, rotation, output = output), coords(producerMirrored, rotation, output = output), 1e-5f, "mirror producer, rotation $rotation")
        }
    }

    @Test
    fun `centre-crops to the output aspect ratio`() {
        val result = coords(flipY, 0, output = 1080 to 1080)
        assertEquals(0.5f - 0.5f * (1080f / 1080f) / (1920f / 1080f), result[0], 1e-5f)
    }

    @Test
    fun `mirrors horizontally`() {
        val result = coords(flipY, 0, mirror = true, output = 1920 to 1080)
        assertEquals(1f, result[0], 1e-6f)
        assertEquals(0f, result[2], 1e-6f)
    }
}

class CameraSelectorTest {
    private val selector = CameraSelector()
    private val main = CameraDescriptor("0", LensFacing.BACK, 90, fieldOfView = 1.0, zoomRatioRange = 0.6f..10f,
        physicalLenses = listOf(PhysicalLens("2", 1.7), PhysicalLens("3", 0.33)))
    private val front = CameraDescriptor("1", LensFacing.FRONT, 270, fieldOfView = 1.1)

    @Test
    fun `picks front and main cameras`() {
        assertEquals(CameraChoice(front, null, CameraSelection.FRONT), selector.choose(CameraSelection.FRONT, listOf(main, front)))
        assertEquals(CameraChoice(main, null, CameraSelection.BACK_WIDE), selector.choose(CameraSelection.BACK_WIDE, listOf(main, front)))
    }

    @Test
    fun `prefers a separate ultra-wide camera, else the zoom ratio`() {
        val ultra = CameraDescriptor("2", LensFacing.BACK, 90, fieldOfView = 1.8)
        assertEquals(CameraChoice(ultra, null, CameraSelection.BACK_ULTRA_WIDE), selector.choose(CameraSelection.BACK_ULTRA_WIDE, listOf(main, front, ultra)))
        assertEquals(CameraChoice(main, 0.6f, CameraSelection.BACK_ULTRA_WIDE), selector.choose(CameraSelection.BACK_ULTRA_WIDE, listOf(main, front)))
    }

    @Test
    fun `reaches the telephoto lens through the zoom ratio`() {
        val choice = selector.choose(CameraSelection.BACK_TELEPHOTO, listOf(main, front))
        assertEquals(CameraSelection.BACK_TELEPHOTO, choice?.actual)
        assertEquals(1.0f / 0.33f, choice?.zoomRatio ?: 0f, 1e-3f)
    }

    @Test
    fun `falls back to back wide without the lens`() {
        val plain = CameraDescriptor("0", LensFacing.BACK, 90, fieldOfView = 1.0)
        assertEquals(CameraChoice(plain, null, CameraSelection.BACK_WIDE), selector.choose(CameraSelection.BACK_TELEPHOTO, listOf(plain)))
        assertEquals(CameraChoice(plain, null, CameraSelection.BACK_WIDE), selector.choose(CameraSelection.FRONT, listOf(plain)))
        assertNull(selector.choose(CameraSelection.BACK_WIDE, emptyList()))
    }
}
