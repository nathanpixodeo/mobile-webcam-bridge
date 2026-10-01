package dev.mobilewebcambridge.protocol.transport

/**
 * Coalesces keyframe requests (host `RequestKeyframe`, scheduler drops) into at most one forced IDR
 * per interval — 2 per second by default (SPEC §3.3).
 *
 * Requests only mark a keyframe as pending; the encoder asks once per frame whether to force it.
 * Not thread-safe; callers that share it between threads must synchronise.
 */
class KeyframeLimiter(val minIntervalUs: Long = 500_000) {
    private var pending = false
    private var lastKeyframeUs: Long? = null

    val isPending: Boolean get() = pending

    fun request() {
        pending = true
    }

    /** Call for every frame about to be encoded. Returns true when this frame must be an IDR. */
    fun shouldForceKeyframe(nowUs: Long): Boolean {
        if (!pending) return false
        val last = lastKeyframeUs
        if (last != null && nowUs - last < minIntervalUs) return false
        pending = false
        lastKeyframeUs = nowUs
        return true
    }

    /** Records a keyframe the encoder produced on its own (first frame, encoder GOP). */
    fun noteKeyframe(nowUs: Long) {
        pending = false
        lastKeyframeUs = nowUs
    }

    fun reset() {
        pending = false
        lastKeyframeUs = null
    }
}
