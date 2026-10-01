package dev.mobilewebcambridge.protocol.time

/**
 * The device clock used for every wire timestamp (SPEC §3). On Android it is
 * `SystemClock.elapsedRealtimeNanos()`, the time base of camera sensor and audio timestamps.
 */
fun interface MonotonicClock {
    fun nowMicros(): Long
}
