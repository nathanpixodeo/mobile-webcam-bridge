package dev.mobilewebcambridge.android.encoding

import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertNull
import org.junit.jupiter.api.Test

class EncoderProfilesTest {
    @Test
    fun `prefers constrained high with its highest level`() {
        val supported = listOf(
            EncoderProfiles.BASELINE to 0x2000,
            EncoderProfiles.HIGH to 0x1000,
            EncoderProfiles.CONSTRAINED_HIGH to 0x800,
            EncoderProfiles.CONSTRAINED_HIGH to 0x2000,
        )
        assertEquals(EncoderProfiles.Choice(EncoderProfiles.CONSTRAINED_HIGH, 0x2000, "constrainedHigh"), EncoderProfiles.choose(supported))
    }

    @Test
    fun `falls back through high, main and baseline`() {
        assertEquals("high", EncoderProfiles.choose(listOf(EncoderProfiles.HIGH to 1, EncoderProfiles.MAIN to 1))?.wireName)
        assertEquals("baseline", EncoderProfiles.choose(listOf(EncoderProfiles.BASELINE to 1))?.wireName)
        assertNull(EncoderProfiles.choose(listOf(0x40 to 1)))
    }
}
