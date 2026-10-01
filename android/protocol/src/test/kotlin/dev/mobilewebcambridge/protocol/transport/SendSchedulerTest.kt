package dev.mobilewebcambridge.protocol.transport

import dev.mobilewebcambridge.protocol.wire.Packet
import dev.mobilewebcambridge.protocol.wire.PacketType
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertNull
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test

class SendSchedulerTest {
    private var seq = 0L

    private fun item(kind: OutboundClass, payloadSize: Int = 10): OutboundItem {
        val type = when (kind) {
            OutboundClass.Control -> PacketType.STATUS
            OutboundClass.Audio -> PacketType.AUDIO_CHUNK
            is OutboundClass.Video -> PacketType.VIDEO_ACCESS_UNIT
            OutboundClass.Log -> PacketType.LOG
        }
        return OutboundItem(Packet.of(type, seq = seq++, timestampUs = 0, payload = ByteArray(payloadSize)), kind)
    }

    private fun video(keyframe: Boolean = false, disposable: Boolean = false) = item(OutboundClass.Video(keyframe, disposable))

    private fun SendScheduler.drainKinds(): List<OutboundClass> {
        val kinds = ArrayList<OutboundClass>()
        while (true) {
            val next = dequeue() ?: break
            kinds += next.kind
            acknowledge(next.bytes.size)
        }
        return kinds
    }

    @Test
    fun `orders control before audio before video before logs`() {
        val scheduler = SendScheduler()
        scheduler.enqueue(item(OutboundClass.Log))
        scheduler.enqueue(video(keyframe = true))
        scheduler.enqueue(item(OutboundClass.Audio))
        scheduler.enqueue(item(OutboundClass.Control))
        assertEquals(
            listOf(OutboundClass.Control, OutboundClass.Audio, OutboundClass.Video(true, false), OutboundClass.Log),
            scheduler.drainKinds(),
        )
    }

    @Test
    fun `respects the in-flight window but always sends one oversized item`() {
        val scheduler = SendScheduler(SendScheduler.Configuration(windowBytes = 100))
        scheduler.enqueue(item(OutboundClass.Control, payloadSize = 500))
        scheduler.enqueue(item(OutboundClass.Control, payloadSize = 10))
        val first = scheduler.dequeue()
        assertEquals(524, first?.bytes?.size)
        assertNull(scheduler.dequeue(), "window full")
        scheduler.acknowledge(524)
        assertEquals(34, scheduler.dequeue()?.bytes?.size)
    }

    @Test
    fun `sheds disposable frames before reference frames`() {
        val scheduler = SendScheduler(SendScheduler.Configuration(maxQueuedVideoFrames = 2))
        scheduler.enqueue(video(keyframe = true))
        scheduler.enqueue(video(disposable = true))
        val result = scheduler.enqueue(video())
        assertEquals(1, result.droppedVideoFrames)
        assertFalse(result.keyframeNeeded)
        assertEquals(2, scheduler.queuedVideoFrames)
    }

    @Test
    fun `drops until the next IDR and asks for one when reference frames must go`() {
        val scheduler = SendScheduler(SendScheduler.Configuration(maxQueuedVideoFrames = 2))
        scheduler.enqueue(video(keyframe = true))
        scheduler.enqueue(video())
        val overflow = scheduler.enqueue(video())
        assertEquals(3, overflow.droppedVideoFrames)
        assertTrue(overflow.keyframeNeeded)
        assertEquals(1, scheduler.enqueue(video()).droppedVideoFrames, "still dropping until IDR")
        assertEquals(0, scheduler.enqueue(video(keyframe = true)).droppedVideoFrames)
        assertEquals(listOf(OutboundClass.Video(true, false)), scheduler.drainKinds())
    }

    @Test
    fun `a new IDR supersedes queued frames`() {
        val scheduler = SendScheduler(SendScheduler.Configuration(maxQueuedVideoFrames = 2))
        scheduler.enqueue(video(keyframe = true))
        scheduler.enqueue(video())
        val result = scheduler.enqueue(video(keyframe = true))
        assertEquals(2, result.droppedVideoFrames)
        assertFalse(result.keyframeNeeded)
        assertEquals(1, scheduler.queuedVideoFrames)
    }

    @Test
    fun `never drops audio but reports a backlog over one second`() {
        val scheduler = SendScheduler()
        var exceeded = false
        repeat(60) { exceeded = scheduler.enqueue(item(OutboundClass.Audio, payloadSize = 1_920)).audioBacklogExceeded }
        assertTrue(exceeded)
        assertEquals(60, scheduler.drainKinds().size)
    }

    @Test
    fun `bounds the log queue`() {
        val scheduler = SendScheduler(SendScheduler.Configuration(maxQueuedLogs = 3))
        repeat(5) { scheduler.enqueue(item(OutboundClass.Log)) }
        assertEquals(3, scheduler.queuedLogCount)
    }

    @Test
    fun `reset forgets everything`() {
        val scheduler = SendScheduler()
        scheduler.enqueue(item(OutboundClass.Audio))
        scheduler.dequeue()
        scheduler.reset()
        assertTrue(scheduler.isIdle)
        assertEquals(0, scheduler.inFlightBytes)
        assertEquals(0, scheduler.queuedAudioBytes)
    }
}

class KeyframeLimiterTest {
    @Test
    fun `forces at most one keyframe per interval`() {
        val limiter = KeyframeLimiter(minIntervalUs = 500_000)
        assertFalse(limiter.shouldForceKeyframe(0))
        limiter.request()
        assertTrue(limiter.shouldForceKeyframe(1_000_000))
        limiter.request()
        assertFalse(limiter.shouldForceKeyframe(1_200_000), "too soon")
        assertTrue(limiter.isPending)
        assertTrue(limiter.shouldForceKeyframe(1_500_000))
        assertFalse(limiter.isPending)
    }

    @Test
    fun `an encoder keyframe satisfies a pending request`() {
        val limiter = KeyframeLimiter()
        limiter.request()
        limiter.noteKeyframe(10)
        assertFalse(limiter.shouldForceKeyframe(10_000_000))
    }
}
