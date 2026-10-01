package dev.mobilewebcambridge.android

import android.app.Application
import android.content.Context
import android.hardware.camera2.CameraManager
import dev.mobilewebcambridge.android.capture.CameraCatalog
import dev.mobilewebcambridge.android.capture.OrientationTracker
import dev.mobilewebcambridge.android.core.AppIdentity
import dev.mobilewebcambridge.android.core.AppLogger
import dev.mobilewebcambridge.android.core.ElapsedRealtimeClock
import dev.mobilewebcambridge.android.system.ExitReasonDiagnostics
import dev.mobilewebcambridge.android.system.PermissionStore
import dev.mobilewebcambridge.android.system.SystemStatusMonitor

class MobileWebcamApplication : Application() {
    lateinit var graph: AppGraph
        private set

    override fun onCreate() {
        super.onCreate()
        graph = AppGraph(this)
        graph.logger.info("app", "Mobile Webcam ${graph.identity.app.version} (${graph.identity.app.build}) started")
        graph.systemMonitor.start()
    }
}

/** Composition root: the app-wide objects. The streaming pipeline itself lives in [StreamingRuntime]. */
class AppGraph(context: Context) {
    val clock = ElapsedRealtimeClock
    val logger = AppLogger(clock)
    val identity = AppIdentity.current(context)
    val permissions = PermissionStore(context)
    val systemMonitor = SystemStatusMonitor(context, permissions, logger)
    val orientation = OrientationTracker(context)
    val cameraCatalog = CameraCatalog(context.getSystemService(CameraManager::class.java))
    val diagnostics = ExitReasonDiagnostics(context)
    val runtime = StreamingRuntime(context, this)
}

val Context.appGraph: AppGraph
    get() = (applicationContext as MobileWebcamApplication).graph
