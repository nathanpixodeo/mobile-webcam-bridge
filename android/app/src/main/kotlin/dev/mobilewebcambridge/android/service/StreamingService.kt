package dev.mobilewebcambridge.android.service

import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import dev.mobilewebcambridge.android.appGraph

/**
 * Foreground service (types `camera` and `microphone`) that keeps the companion server, the camera
 * and the microphone running while the screen is off or another app is in front. Started from the
 * activity (Android only allows camera and microphone foreground services to start while the app
 * is visible); stopped from the activity or the notification.
 */
class StreamingService : Service() {
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onCreate() {
        super.onCreate()
        StreamingNotification.ensureChannel(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val graph = appGraph
        if (intent?.action == ACTION_STOP) {
            graph.logger.info("service", "Stopped from the notification")
            stopSelf()
            return START_NOT_STICKY
        }
        val types = foregroundTypes()
        if (types == 0) {
            graph.logger.warn("service", "Neither camera nor microphone access is granted; not starting")
            stopSelf()
            return START_NOT_STICKY
        }
        try {
            ServiceCompat.startForeground(this, StreamingNotification.ID, StreamingNotification.build(this), types)
        } catch (error: Exception) {
            // ForegroundServiceStartNotAllowedException or SecurityException (e.g. started from the background).
            graph.logger.error("service", "Cannot start the foreground service: ${error.message}")
            stopSelf()
            return START_NOT_STICKY
        }
        acquireWakeLock()
        graph.runtime.start()
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        appGraph.runtime.stop()
        wakeLock?.takeIf { it.isHeld }?.release()
        wakeLock = null
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    /** Only the types whose permission is granted: Android 14+ rejects the others. */
    private fun foregroundTypes(): Int {
        val permissions = appGraph.permissions
        var types = 0
        if (permissions.hasCamera) types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
        if (permissions.hasMicrophone) types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
        return types
    }

    /** Keeps the CPU running with the screen off; the camera and encoder need it. */
    private fun acquireWakeLock() {
        if (wakeLock?.isHeld == true) return
        wakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MobileWebcam:stream")
            .apply {
                setReferenceCounted(false)
                acquire()
            }
    }

    companion object {
        const val ACTION_STOP = "dev.mobilewebcambridge.android.action.STOP"

        fun start(context: Context) {
            ContextCompat.startForegroundService(context, Intent(context, StreamingService::class.java))
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, StreamingService::class.java))
        }
    }
}
