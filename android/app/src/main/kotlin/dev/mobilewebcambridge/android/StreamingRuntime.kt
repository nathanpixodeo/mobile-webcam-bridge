package dev.mobilewebcambridge.android

import android.content.Context
import dev.mobilewebcambridge.android.capture.CameraVideoSource
import dev.mobilewebcambridge.android.capture.MicAudioSource
import dev.mobilewebcambridge.android.capture.SyntheticAudioSource
import dev.mobilewebcambridge.android.capture.SyntheticVideoSource
import dev.mobilewebcambridge.android.core.SerialThread
import dev.mobilewebcambridge.android.encoding.H264Encoder
import dev.mobilewebcambridge.android.media.AudioStreamController
import dev.mobilewebcambridge.android.media.MediaController
import dev.mobilewebcambridge.android.media.VideoStreamController
import dev.mobilewebcambridge.android.session.SessionCoordinator
import dev.mobilewebcambridge.android.session.SessionSnapshot
import dev.mobilewebcambridge.android.system.BootReport
import dev.mobilewebcambridge.android.transport.CompanionServer
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * Builds, starts and stops the streaming pipeline: companion server, session, video and audio
 * pipelines, each on its own thread. Owned by [AppGraph], driven by the foreground service.
 * Main thread only.
 */
class StreamingRuntime(private val context: Context, private val graph: AppGraph) {
    private val running = MutableStateFlow(false)
    private val synthetic = MutableStateFlow(false)
    private val sessionSnapshot = MutableStateFlow(SessionSnapshot())
    private var active: Pipeline? = null

    val isRunning: StateFlow<Boolean> = running.asStateFlow()
    val usesTestPattern: StateFlow<Boolean> = synthetic.asStateFlow()
    val snapshot: StateFlow<SessionSnapshot> = sessionSnapshot.asStateFlow()

    fun start() {
        if (active != null) return
        graph.logger.info("app", "Starting the companion server on port $PORT")
        active = Pipeline().also { it.start() }
        running.value = true
    }

    fun stop() {
        val pipeline = active ?: return
        active = null
        graph.logger.info("app", "Stopping")
        pipeline.stop()
        running.value = false
        sessionSnapshot.value = SessionSnapshot()
    }

    /** Replaces the camera and microphone with a test pattern and a tone (diagnostics). */
    fun setTestPattern(enabled: Boolean) {
        synthetic.value = enabled
        active?.setSynthetic(enabled)
    }

    /** One run of the pipeline, from start to stop. */
    private inner class Pipeline {
        private val sessionThread = SerialThread("mwb-session")
        private val videoThread = SerialThread("mwb-video")
        private val audioThread = SerialThread("mwb-audio")
        private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
        private val logger = graph.logger

        private val coordinator = SessionCoordinator(
            thread = sessionThread,
            identity = graph.identity,
            clock = graph.clock,
            logger = logger,
            diagnostics = graph.diagnostics,
            bootReport = BootReport.collect(graph.cameraCatalog),
        )
        private val video = VideoStreamController(
            thread = videoThread,
            camera = CameraVideoSource(context, graph.cameraCatalog, graph.orientation, logger),
            synthetic = SyntheticVideoSource(graph.clock),
            encoders = H264Encoder.Factory(logger),
            sink = coordinator,
            logger = logger,
        )
        private val audio = AudioStreamController(
            thread = audioThread,
            microphone = MicAudioSource(context, logger),
            synthetic = SyntheticAudioSource(graph.clock),
            sink = coordinator,
            logger = logger,
        )
        private val server = CompanionServer(
            port = PORT,
            logger = logger,
            onState = { coordinator.serverStateChanged(it) },
            onAccept = { coordinator.accept(it) },
        )

        fun start() {
            coordinator.bind(MediaController(video, audio))
            setSynthetic(synthetic.value)
            coordinator.start()
            graph.orientation.start()
            scope.launch {
                graph.systemMonitor.snapshot.collect { system ->
                    coordinator.updateSystem(system)
                    video.setThermalLevel(system.thermal)
                }
            }
            scope.launch { coordinator.snapshot.collect { sessionSnapshot.value = it } }
            server.start()
        }

        fun setSynthetic(enabled: Boolean) {
            video.setSyntheticSource(enabled)
            audio.setSyntheticSource(enabled)
        }

        /**
         * Stops accepting, closes the connection and the media pipelines, waits until the camera
         * and microphone are released, then ends the threads.
         */
        fun stop() {
            server.stop()
            coordinator.shutdown()
            video.stop() // idempotent; covers media the session no longer tracks
            audio.stop()
            scope.cancel()
            graph.orientation.stop()
            // Each call() waits for the work queued before it; the session posts the media stops.
            for (thread in listOf(sessionThread, videoThread, audioThread)) {
                runCatching { thread.call(timeoutMs = STOP_TIMEOUT_MS) { } }
                    .onFailure { logger.warn("app", "${thread.name} did not stop in time: ${it.message}") }
                thread.quit()
            }
        }
    }

    companion object {
        /** SPEC §1: the port the host reaches through ADB (`tcp:27100`). */
        const val PORT = 27100
        private const val STOP_TIMEOUT_MS = 3_000L
    }
}
