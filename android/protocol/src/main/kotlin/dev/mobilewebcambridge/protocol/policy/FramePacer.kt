package dev.mobilewebcambridge.protocol.policy

/**
 * Thins a frame stream down to a target rate. Cameras often run a fixed or variable rate above what
 * the host asked for (or above a thermal cap); frames closer than the target interval to the last
 * emitted frame are skipped. A small tolerance keeps exact-rate streams from losing frames to
 * timestamp jitter.
 */
class FramePacer(targetFps: Int) {
    private val intervalUs: Long = 1_000_000L / targetFps.coerceAtLeast(1)
    private val toleranceUs: Long = intervalUs / 5
    private var nextDueUs: Long? = null

    /** Returns true when the frame captured at [timestampUs] should be encoded. */
    fun shouldEmit(timestampUs: Long): Boolean {
        val due = nextDueUs
        if (due != null && timestampUs < due - toleranceUs) return false
        // Schedule from the ideal time, not the actual one, so the average rate stays exact; but
        // never fall more than one interval behind after a gap.
        nextDueUs = if (due == null || timestampUs - due > intervalUs) timestampUs + intervalUs else due + intervalUs
        return true
    }

    fun reset() {
        nextDueUs = null
    }
}
