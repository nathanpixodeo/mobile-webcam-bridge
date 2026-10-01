package dev.mobilewebcambridge.protocol.policy

import dev.mobilewebcambridge.protocol.messages.ThermalStateName

/** Platform-neutral thermal level; declaration order is severity order. */
enum class ThermalLevel {
    NOMINAL,
    FAIR,
    SERIOUS,
    CRITICAL,
    SHUTDOWN,
    ;

    /** Name reported in `Status.thermal`; the wire format has no `shutdown`. */
    val statusName: ThermalStateName
        get() = when (this) {
            NOMINAL -> ThermalStateName.NOMINAL
            FAIR -> ThermalStateName.FAIR
            SERIOUS -> ThermalStateName.SERIOUS
            CRITICAL, SHUTDOWN -> ThermalStateName.CRITICAL
        }
}

/** Video limits imposed by a thermal level. */
data class VideoThrottle(
    val maxFps: Int?,
    /** Bitrate multiplier in per mille (1000 = unchanged). */
    val bitratePermille: Int,
    val videoAllowed: Boolean,
) {
    companion object {
        val UNRESTRICTED: VideoThrottle = VideoThrottle(maxFps = null, bitratePermille = 1_000, videoAllowed = true)
    }
}

data class EffectiveVideoRate(val fps: Int, val bitrateKbps: Int)

/**
 * Thermal throttling table shared with iOS: serious → 24 fps and 70 % bitrate, critical → 15 fps
 * and 50 %, shutdown → no video.
 */
class ThermalPolicy {
    fun throttle(level: ThermalLevel): VideoThrottle = when (level) {
        ThermalLevel.NOMINAL, ThermalLevel.FAIR -> VideoThrottle.UNRESTRICTED
        ThermalLevel.SERIOUS -> VideoThrottle(maxFps = 24, bitratePermille = 700, videoAllowed = true)
        ThermalLevel.CRITICAL -> VideoThrottle(maxFps = 15, bitratePermille = 500, videoAllowed = true)
        ThermalLevel.SHUTDOWN -> VideoThrottle(maxFps = null, bitratePermille = 0, videoAllowed = false)
    }

    /** Effective frame rate and bitrate after throttling. */
    fun apply(throttle: VideoThrottle, fps: Int, bitrateKbps: Int): EffectiveVideoRate {
        val cappedFps = throttle.maxFps?.let { minOf(fps, it) } ?: fps
        val scaledBitrate = bitrateKbps * throttle.bitratePermille / 1_000
        return EffectiveVideoRate(fps = cappedFps.coerceAtLeast(1), bitrateKbps = scaledBitrate.coerceAtLeast(250))
    }
}
