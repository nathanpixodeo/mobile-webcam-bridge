package dev.mobilewebcambridge.android.capture

import android.os.Process
import android.os.SystemClock
import android.view.Surface
import dev.mobilewebcambridge.android.core.SerialThread
import dev.mobilewebcambridge.android.gl.EglCore
import dev.mobilewebcambridge.android.gl.FrameRenderer
import dev.mobilewebcambridge.protocol.media.AudioChunk
import dev.mobilewebcambridge.protocol.media.Pcm16
import dev.mobilewebcambridge.protocol.media.ToneGenerator
import dev.mobilewebcambridge.protocol.messages.OrientationMode
import dev.mobilewebcambridge.protocol.messages.StartAudioMessage
import dev.mobilewebcambridge.protocol.time.MonotonicClock

/**
 * Test pattern for debugging without a camera: colour bars, a moving bar, a latency flash and a
 * binary frame counter (see [FrameRenderer.drawTestPattern]), drawn with GL into the encoder
 * surface at the requested rate. Always upright; portrait only when the host asks for portrait.
 */
class SyntheticVideoSource(private val clock: MonotonicClock) : VideoFrameSource {
    private var prepared: VideoCaptureFormat? = null
    private var activeRun: PatternRun? = null

    override fun prepare(request: VideoCaptureRequest): VideoCaptureFormat {
        val portrait = request.orientation == OrientationMode.PORTRAIT
        return VideoCaptureFormat(
            width = (if (portrait) request.height else request.width) and 1.inv(),
            height = (if (portrait) request.width else request.height) and 1.inv(),
            fps = request.fps,
            rotationDegrees = 0,
            mirrored = false,
            camera = request.camera,
        ).also { prepared = it }
    }

    override fun start(target: Surface, fps: Int, hook: FrameHook, listener: VideoSourceListener) {
        val format = checkNotNull(prepared) { "prepare() must run before start()" }
        activeRun?.stop()
        activeRun = null
        activeRun = PatternRun(format, target, fps, hook, listener).also { it.start() }
    }

    override fun setFrameRate(fps: Int) {
        activeRun?.setFrameRate(fps)
    }

    override fun stop() {
        activeRun?.stop()
        activeRun = null
    }

    private inner class PatternRun(
        private val format: VideoCaptureFormat,
        private val target: Surface,
        private var fps: Int, // render thread after start()
        private val hook: FrameHook,
        private val listener: VideoSourceListener,
    ) {
        private val thread = SerialThread("mwb-pattern", Process.THREAD_PRIORITY_URGENT_DISPLAY)
        private val tick = Runnable { drawFrame() }
        private var egl: EglCore? = null
        private var renderer: FrameRenderer? = null
        private var frameIndex = 0L

        // Frame deadlines are computed from a fixed origin, so the long-term rate is exact.
        private var originMs = 0L
        private var scheduled = 0L

        @Volatile
        private var stopped = false
        private var released = false // video thread

        fun start() {
            try {
                thread.call {
                    egl = EglCore(target)
                    renderer = FrameRenderer(format.width, format.height)
                    restartTimeline()
                }
            } catch (error: Exception) {
                stop()
                throw CaptureException.encoderFailure("Cannot render the test pattern: ${error.message}")
            }
        }

        fun setFrameRate(fps: Int) {
            thread.post {
                this.fps = fps
                restartTimeline()
            }
        }

        fun stop() {
            if (released) return
            released = true
            stopped = true
            runCatching {
                thread.call {
                    thread.cancel(tick)
                    renderer?.release()
                    egl?.release()
                    renderer = null
                    egl = null
                }
            }
            thread.quit()
        }

        private fun restartTimeline() {
            thread.cancel(tick)
            originMs = SystemClock.uptimeMillis()
            scheduled = 0
            thread.handler.postAtTime(tick, originMs)
        }

        private fun drawFrame() {
            if (stopped) return
            val core = egl ?: return
            val frameRenderer = renderer ?: return
            try {
                val timestampUs = clock.nowMicros()
                hook.beforeFrame(timestampUs)
                frameRenderer.drawTestPattern(timestampUs, frameIndex)
                core.setPresentationTime(timestampUs * 1_000)
                check(core.swapBuffers()) { "the encoder surface was abandoned" }
            } catch (error: Exception) {
                stopped = true
                listener.onSourceEvent(SourceEvent.Failed("Test pattern rendering failed: ${error.message}"))
                return
            }
            frameIndex += 1
            scheduled += 1
            val dueMs = originMs + scheduled * 1_000L / fps.coerceAtLeast(1)
            if (SystemClock.uptimeMillis() - dueMs > 1_000) {
                restartTimeline() // fell far behind (e.g. a long GC pause): do not burst to catch up
            } else {
                thread.handler.postAtTime(tick, dueMs)
            }
        }
    }
}

/**
 * 440 Hz tone at -12 dBFS in 20 ms blocks, paced by the clock so the long-term rate is exactly
 * 48 kHz (a stand-in microphone for testing the host's audio path).
 */
class SyntheticAudioSource(private val clock: MonotonicClock) : AudioSampleSource {
    private var activeRun: ToneRun? = null

    override fun start(request: StartAudioMessage, listener: AudioSourceListener) {
        stop()
        activeRun = ToneRun(listener).also { it.start() }
    }

    override fun stop() {
        activeRun?.stop()
        activeRun = null
    }

    private inner class ToneRun(private val listener: AudioSourceListener) {
        private val thread = SerialThread("mwb-tone", Process.THREAD_PRIORITY_URGENT_AUDIO)
        private val generator = ToneGenerator()
        private val tick = Runnable { produce() }
        private var startUs = 0L
        private var producedSamples = 0L

        @Volatile
        private var stopped = false

        fun start() {
            thread.post {
                startUs = clock.nowMicros()
                producedSamples = 0
                thread.handler.post(tick)
            }
        }

        fun stop() {
            stopped = true
            runCatching { thread.call { thread.cancel(tick) } }
            thread.quit()
        }

        private fun produce() {
            if (stopped) return
            val elapsedUs = clock.nowMicros() - startUs
            val due = elapsedUs * AudioChunk.SAMPLE_RATE / 1_000_000 - producedSamples
            if (due > 0) {
                val timestampUs = startUs + producedSamples * 1_000_000 / AudioChunk.SAMPLE_RATE
                val samples = generator.next(due.toInt())
                listener.onAudioCaptured(Pcm16.toLittleEndian(samples), timestampUs, discontinuity = producedSamples == 0L)
                producedSamples += due
            }
            thread.postDelayed(TICK_MS, tick)
        }
    }

    private companion object {
        const val TICK_MS = 20L
    }
}
