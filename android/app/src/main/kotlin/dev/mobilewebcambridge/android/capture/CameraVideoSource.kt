package dev.mobilewebcambridge.android.capture

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.os.Build
import android.os.Process
import android.os.SystemClock
import android.util.Range
import android.view.Surface
import androidx.core.content.ContextCompat
import dev.mobilewebcambridge.android.core.AppLogger
import dev.mobilewebcambridge.android.core.SerialThread
import dev.mobilewebcambridge.android.gl.EglCore
import dev.mobilewebcambridge.android.gl.FrameRenderer
import dev.mobilewebcambridge.protocol.policy.FormatSelector
import dev.mobilewebcambridge.protocol.policy.FramePacer
import kotlin.math.roundToInt

/**
 * Camera2 capture rendered through OpenGL into the encoder surface. GL does the work a raw
 * camera→encoder connection cannot: rotate to the phone's pose, centre-crop to the requested size,
 * mirror, and pace to the requested frame rate.
 *
 * Threads: `prepare`/`start`/`stop` run on the video thread; Camera2 callbacks on a camera
 * thread; frames on a render thread that owns the EGL context. The source reports interruptions
 * and pose changes; the video controller decides what to do with them.
 */
class CameraVideoSource(
    private val context: Context,
    private val catalog: CameraCatalog,
    private val orientation: OrientationTracker,
    private val logger: AppLogger,
) : VideoFrameSource {
    private val manager: CameraManager = context.getSystemService(CameraManager::class.java)
    private val selector = CameraSelector()
    private val formats = FormatSelector()
    private var plan: Plan? = null
    private var activeRun: CaptureRun? = null

    /** Everything decided by [prepare] for one pipeline run. */
    private data class Plan(
        val request: VideoCaptureRequest,
        val choice: CameraChoice,
        val captureWidth: Int,
        val captureHeight: Int,
        val fpsRange: Range<Int>?,
        val deviceOrientation: Int,
        val format: VideoCaptureFormat,
    )

    override fun prepare(request: VideoCaptureRequest): VideoCaptureFormat {
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            throw CaptureException.permissionDenied("Camera permission has not been granted")
        }
        val choice = selector.choose(request.camera, catalog.describe())
            ?: throw CaptureException.cameraUnavailable("This device has no usable camera")
        if (choice.actual != request.camera) {
            logger.warn("video", "Camera ${request.camera.wire} unavailable, using ${choice.actual.wire}")
        }
        val capture = formats.select(catalog.surfaceFormats(choice.camera.id), request.width, request.height, request.fps)
            ?: throw CaptureException.cameraUnavailable("Camera ${choice.camera.id} offers no output size")
        val fpsRange = catalog.frameRateRange(choice.camera.id, request.fps)
        val fps = minOf(request.fps, fpsRange?.upper ?: request.fps, capture.maxFrameRate.roundToInt().coerceAtLeast(1))
        val pose = orientation.current
        val format = formatFor(request, choice, pose, fps)
        plan = Plan(request, choice, capture.width, capture.height, fpsRange, pose, format)
        logger.info(
            "video",
            "Camera ${choice.camera.id} (${choice.actual.wire}${choice.zoomRatio?.let { " zoom ${"%.2f".format(it)}" } ?: ""}) " +
                "capture ${capture.width}x${capture.height}, output ${format.width}x${format.height}@$fps, rotation ${format.rotationDegrees}",
        )
        return format
    }

    override fun start(target: Surface, fps: Int, hook: FrameHook, listener: VideoSourceListener) {
        val plan = checkNotNull(plan) { "prepare() must run before start()" }
        activeRun?.stop()
        activeRun = null
        activeRun = CaptureRun(plan, target, fps, hook, listener).also { it.start() }
    }

    override fun setFrameRate(fps: Int) {
        activeRun?.setFrameRate(fps)
    }

    override fun stop() {
        activeRun?.stop()
        activeRun = null
    }

    private fun formatFor(request: VideoCaptureRequest, choice: CameraChoice, pose: Int, fps: Int): VideoCaptureFormat {
        val rotation = RotationMath.imageRotation(choice.camera.sensorOrientation, pose, choice.camera.facing == LensFacing.FRONT)
        val landscape = RotationMath.isLandscapeOutput(request.orientation, pose)
        // H.264 encoders need even dimensions (4:2:0 chroma).
        return VideoCaptureFormat(
            width = (if (landscape) request.width else request.height) and 1.inv(),
            height = (if (landscape) request.height else request.width) and 1.inv(),
            fps = fps,
            rotationDegrees = rotation,
            mirrored = request.mirror,
            camera = choice.actual,
        )
    }

    /** One camera session rendering into one encoder surface. */
    private inner class CaptureRun(
        private val plan: Plan,
        private val target: Surface,
        fps: Int,
        private val hook: FrameHook,
        private val listener: VideoSourceListener,
    ) {
        private val renderThread = SerialThread("mwb-render", Process.THREAD_PRIORITY_URGENT_DISPLAY)
        private val cameraThread = SerialThread("mwb-camera")
        private var pacer = FramePacer(fps) // render thread
        private val stMatrix = FloatArray(16)

        /** Camera timestamps on the monotonic clock are moved onto `elapsedRealtime`. */
        private val timestampOffsetNs: Long =
            if (plan.choice.camera.realtimeTimestamps) 0 else SystemClock.elapsedRealtimeNanos() - System.nanoTime()

        // Render-thread state.
        private var egl: EglCore? = null
        private var renderer: FrameRenderer? = null
        private var textureId = 0
        private var surfaceTexture: SurfaceTexture? = null
        private var cameraSurface: Surface? = null

        // Camera-thread state.
        private var device: CameraDevice? = null
        private var captureSession: CameraCaptureSession? = null
        private var interrupted = false
        private var failedOpens = 0

        @Volatile
        private var stopped = false
        private var released = false // video thread
        private var removeOrientationListener: (() -> Unit)? = null

        private val availability = object : CameraManager.AvailabilityCallback() {
            override fun onCameraAvailable(cameraId: String) {
                if (cameraId == plan.choice.camera.id && interrupted && device == null && !stopped) openCamera()
            }
        }

        fun start() {
            try {
                renderThread.call {
                    egl = EglCore(target)
                    val frameRenderer = FrameRenderer(plan.format.width, plan.format.height)
                    renderer = frameRenderer
                    textureId = frameRenderer.createExternalTexture()
                    val st = SurfaceTexture(textureId).apply {
                        setDefaultBufferSize(plan.captureWidth, plan.captureHeight)
                        setOnFrameAvailableListener({ drawFrame() }, renderThread.handler)
                    }
                    surfaceTexture = st
                    cameraSurface = Surface(st)
                }
            } catch (error: Exception) {
                stop()
                throw CaptureException.cameraUnavailable("Cannot set up camera rendering: ${error.message}")
            }
            manager.registerAvailabilityCallback(availability, cameraThread.handler)
            cameraThread.post { openCamera() }
            removeOrientationListener = orientation.addListener { pose -> onPoseChanged(pose) }
        }

        fun setFrameRate(fps: Int) {
            renderThread.post { pacer = FramePacer(fps) }
        }

        fun stop() {
            if (released) return
            released = true
            stopped = true
            removeOrientationListener?.invoke()
            manager.unregisterAvailabilityCallback(availability)
            runCatching {
                cameraThread.call {
                    captureSession?.close()
                    device?.close()
                    captureSession = null
                    device = null
                }
            }.onFailure { logger.warn("video", "Camera close: ${it.message}") }
            runCatching {
                renderThread.call {
                    surfaceTexture?.setOnFrameAvailableListener(null)
                    cameraSurface?.release()
                    surfaceTexture?.release()
                    renderer?.let {
                        it.deleteTexture(textureId)
                        it.release()
                    }
                    egl?.release()
                    surfaceTexture = null
                    cameraSurface = null
                    renderer = null
                    egl = null
                }
            }.onFailure { logger.warn("video", "Renderer release: ${it.message}") }
            cameraThread.quit()
            renderThread.quit()
        }

        // ---- Render thread ------------------------------------------------------------------

        private fun drawFrame() {
            if (stopped) return
            val st = surfaceTexture ?: return
            val core = egl ?: return
            val frameRenderer = renderer ?: return
            try {
                st.updateTexImage() // always consume the buffer, or the camera stalls
                val timestampNs = st.timestamp + timestampOffsetNs
                val timestampUs = timestampNs / 1_000
                if (!pacer.shouldEmit(timestampUs)) return
                st.getTransformMatrix(stMatrix)
                val coords = TextureMapping.cornerTexCoords(
                    stMatrix,
                    plan.format.rotationDegrees,
                    plan.format.mirrored,
                    plan.captureWidth,
                    plan.captureHeight,
                    plan.format.width,
                    plan.format.height,
                )
                hook.beforeFrame(timestampUs)
                frameRenderer.drawExternal(textureId, coords)
                core.setPresentationTime(timestampNs)
                check(core.swapBuffers()) { "the encoder surface was abandoned" }
            } catch (error: Exception) {
                // An exception escaping a HandlerThread kills the app; report it instead.
                stopped = true
                listener.onSourceEvent(SourceEvent.Failed("Camera rendering failed: ${error.message}"))
            }
        }

        // ---- Camera thread ------------------------------------------------------------------

        @SuppressLint("MissingPermission") // checked in prepare(); re-checked here before every open
        private fun openCamera() {
            if (stopped) return
            if (ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
                listener.onSourceEvent(SourceEvent.Failed("Camera permission was revoked"))
                return
            }
            try {
                manager.openCamera(plan.choice.camera.id, stateCallback, cameraThread.handler)
            } catch (error: Exception) {
                listener.onSourceEvent(SourceEvent.Failed("Cannot open camera ${plan.choice.camera.id}: ${error.message}"))
            }
        }

        private val stateCallback = object : CameraDevice.StateCallback() {
            override fun onOpened(camera: CameraDevice) {
                if (stopped) {
                    camera.close()
                    return
                }
                device = camera
                failedOpens = 0
                createSession(camera)
            }

            override fun onDisconnected(camera: CameraDevice) {
                camera.close()
                device = null
                captureSession = null
                if (stopped) return
                interrupted = true
                logger.warn("video", "Camera disconnected (another app took it)")
                listener.onSourceEvent(SourceEvent.Interrupted("inUseByAnotherClient"))
            }

            override fun onError(camera: CameraDevice, error: Int) {
                camera.close()
                device = null
                captureSession = null
                if (stopped) return
                logger.warn("video", "Camera error $error")
                when (error) {
                    ERROR_CAMERA_DISABLED -> listener.onSourceEvent(SourceEvent.Failed("The camera is disabled by device policy"))
                    ERROR_CAMERA_IN_USE, ERROR_MAX_CAMERAS_IN_USE -> {
                        // Reopened by the availability callback once the other client lets go.
                        interrupted = true
                        listener.onSourceEvent(SourceEvent.Interrupted("inUseByAnotherClient"))
                    }
                    else -> {
                        failedOpens += 1
                        if (failedOpens > MAX_FAILED_OPENS) {
                            listener.onSourceEvent(SourceEvent.Failed("The camera keeps failing (error $error)"))
                            return
                        }
                        if (!interrupted) listener.onSourceEvent(SourceEvent.Interrupted("cameraError"))
                        interrupted = true
                        cameraThread.postDelayed(RETRY_DELAY_MS, Runnable { if (device == null && !stopped) openCamera() })
                    }
                }
            }
        }

        private fun createSession(camera: CameraDevice) {
            val surface = cameraSurface ?: return
            val callback = object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    if (stopped) {
                        session.close()
                        return
                    }
                    captureSession = session
                    startRepeating(camera, session, surface)
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                    listener.onSourceEvent(SourceEvent.Failed("The camera rejected the session configuration"))
                }
            }
            try {
                val configuration = SessionConfiguration(
                    SessionConfiguration.SESSION_REGULAR,
                    listOf(OutputConfiguration(surface)),
                    cameraThread.executor,
                    callback,
                )
                camera.createCaptureSession(configuration)
            } catch (error: Exception) {
                listener.onSourceEvent(SourceEvent.Failed("Cannot create the camera session: ${error.message}"))
            }
        }

        private fun startRepeating(camera: CameraDevice, session: CameraCaptureSession, surface: Surface) {
            try {
                val characteristics = manager.getCameraCharacteristics(plan.choice.camera.id)
                val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                    addTarget(surface)
                    set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO)
                    plan.fpsRange?.let { set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, it) }
                    // Stabilisation buffers frames; a webcam on a stand gains nothing from it.
                    set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_OFF)
                    val afModes = characteristics.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES) ?: IntArray(0)
                    if (CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO in afModes) {
                        set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                    }
                    val zoom = plan.choice.zoomRatio
                    if (zoom != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                        set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom)
                    }
                }.build()
                session.setRepeatingRequest(request, null, cameraThread.handler)
                if (interrupted) {
                    interrupted = false
                    logger.info("video", "Camera resumed")
                    listener.onSourceEvent(SourceEvent.Resumed)
                }
            } catch (error: Exception) {
                listener.onSourceEvent(SourceEvent.Failed("Cannot start the camera: ${error.message}"))
            }
        }

        // ---- Main thread (orientation) ------------------------------------------------------

        private fun onPoseChanged(pose: Int) {
            if (stopped || pose == plan.deviceOrientation) return
            val updated = formatFor(plan.request, plan.choice, pose, plan.format.fps)
            if (updated != plan.format) listener.onSourceEvent(SourceEvent.FormatChanged(updated))
        }
    }

    private companion object {
        const val RETRY_DELAY_MS = 1_000L
        const val MAX_FAILED_OPENS = 5
    }
}
