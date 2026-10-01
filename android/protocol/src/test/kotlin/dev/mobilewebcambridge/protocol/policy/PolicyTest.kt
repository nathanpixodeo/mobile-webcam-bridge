package dev.mobilewebcambridge.protocol.policy

import dev.mobilewebcambridge.protocol.messages.ThermalStateName
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertNull
import org.junit.jupiter.api.Test

class ThermalPolicyTest {
    private val policy = ThermalPolicy()

    @Test
    fun `throttles by level`() {
        assertEquals(EffectiveVideoRate(30, 6000), policy.apply(policy.throttle(ThermalLevel.FAIR), 30, 6000))
        assertEquals(EffectiveVideoRate(24, 4200), policy.apply(policy.throttle(ThermalLevel.SERIOUS), 30, 6000))
        assertEquals(EffectiveVideoRate(15, 3000), policy.apply(policy.throttle(ThermalLevel.CRITICAL), 60, 6000))
        assertFalse(policy.throttle(ThermalLevel.SHUTDOWN).videoAllowed)
        assertEquals(ThermalStateName.CRITICAL, ThermalLevel.SHUTDOWN.statusName)
    }

    @Test
    fun `keeps a minimum bitrate`() {
        assertEquals(250, policy.apply(policy.throttle(ThermalLevel.CRITICAL), 30, 400).bitrateKbps)
    }
}

class FormatSelectorTest {
    private val selector = FormatSelector()
    private fun format(index: Int, width: Int, height: Int, maxFps: Double = 30.0, minFps: Double = 1.0) =
        CaptureFormatInfo(index, width, height, minFps, maxFps)

    @Test
    fun `prefers the exact size with the lowest maximum rate`() {
        val formats = listOf(format(0, 1920, 1080, 60.0), format(1, 1280, 720, 60.0), format(2, 1280, 720, 30.0))
        assertEquals(2, selector.select(formats, 1280, 720, 30)?.index)
    }

    @Test
    fun `falls back to the smallest larger format of the same aspect`() {
        val formats = listOf(format(0, 4032, 3024), format(1, 3840, 2160), format(2, 1920, 1080), format(3, 1440, 1080))
        assertEquals(2, selector.select(formats, 1280, 720, 30)?.index)
    }

    @Test
    fun `then the smallest covering format, then the largest`() {
        assertEquals(1, selector.select(listOf(format(0, 4032, 3024), format(1, 1440, 1080)), 1280, 720, 30)?.index)
        assertEquals(0, selector.select(listOf(format(0, 640, 480), format(1, 320, 240)), 1280, 720, 30)?.index)
    }

    @Test
    fun `ignores formats that cannot reach the frame rate when others can`() {
        val formats = listOf(format(0, 1280, 720, maxFps = 24.0), format(1, 1920, 1080, maxFps = 60.0))
        assertEquals(1, selector.select(formats, 1280, 720, 60)?.index)
        assertNull(selector.select(emptyList(), 1280, 720, 30))
    }
}

class FramePacerTest {
    @Test
    fun `thins a 60 fps stream to 30 fps`() {
        val pacer = FramePacer(targetFps = 30)
        val emitted = (0 until 60).count { frame -> pacer.shouldEmit(frame * 16_667L) }
        assertEquals(30, emitted)
    }

    @Test
    fun `keeps every frame of an exact-rate stream despite jitter`() {
        val pacer = FramePacer(targetFps = 30)
        val jitter = longArrayOf(0, 3_000, -3_000, 1_500, -2_000)
        val emitted = (0 until 100).count { frame -> pacer.shouldEmit(frame * 33_333L + jitter[frame % jitter.size]) }
        assertEquals(100, emitted)
    }

    @Test
    fun `resynchronises after a gap`() {
        val pacer = FramePacer(targetFps = 30)
        pacer.shouldEmit(0)
        pacer.shouldEmit(5_000_000)
        assertFalse(pacer.shouldEmit(5_010_000))
        assertEquals(true, pacer.shouldEmit(5_033_333))
    }
}
