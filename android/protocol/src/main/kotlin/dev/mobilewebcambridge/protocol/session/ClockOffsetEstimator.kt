package dev.mobilewebcambridge.protocol.session

/** One Ping/Pong exchange, evaluated NTP style (SPEC §3.1). */
data class ClockSample(
    val roundTripUs: Long,
    /** Peer clock minus local clock. */
    val offsetUs: Long,
)

/**
 * Keeps the last [windowSize] samples and trusts the one with the lowest round trip, which is the
 * least disturbed by queuing delay.
 */
class ClockOffsetEstimator(windowSize: Int = 16) {
    val windowSize: Int = windowSize.coerceAtLeast(1)
    private val samples = ArrayDeque<ClockSample>()

    /** [t0] local send, [t1] peer receive, [t2] peer send, [t3] local receive. */
    fun add(t0: Long, t1: Long, t2: Long, t3: Long): ClockSample? {
        val roundTrip = (t3 - t0) - (t2 - t1)
        if (roundTrip < 0) return null
        val sample = ClockSample(roundTripUs = roundTrip, offsetUs = ((t1 - t0) + (t2 - t3)) / 2)
        samples.addLast(sample)
        while (samples.size > windowSize) samples.removeFirst()
        return sample
    }

    val best: ClockSample? get() = samples.minByOrNull { it.roundTripUs }

    fun reset() {
        samples.clear()
    }
}
