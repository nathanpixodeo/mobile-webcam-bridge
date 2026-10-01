package dev.mobilewebcambridge.android.capture

import android.content.Context
import android.hardware.SensorManager
import android.os.Handler
import android.os.Looper
import android.view.OrientationEventListener
import java.util.concurrent.CopyOnWriteArraySet

/**
 * Physical device orientation (0/90/180/270, clockwise from natural), independent of the screen,
 * which may be off or locked to portrait while streaming. A new orientation is only reported after
 * it has been stable for [settleMs], so lifting the phone does not restart the camera pipeline.
 * Create and use on the main thread; listeners are called on the main thread.
 */
class OrientationTracker(context: Context, private val settleMs: Long = 600) {
    @Volatile
    var current: Int = 0
        private set

    private val listeners = CopyOnWriteArraySet<(Int) -> Unit>()
    private val mainHandler = Handler(Looper.getMainLooper())
    private var candidate: Int = 0
    private val commit = Runnable {
        if (candidate != current) {
            current = candidate
            listeners.forEach { it(current) }
        }
    }

    private val sensorListener = object : OrientationEventListener(context, SensorManager.SENSOR_DELAY_NORMAL) {
        override fun onOrientationChanged(orientation: Int) {
            if (orientation == OrientationEventListener.ORIENTATION_UNKNOWN) return // lying flat: keep the last pose
            val quantized = RotationMath.quantize(orientation, previous = candidate)
            if (quantized == candidate) return
            candidate = quantized
            mainHandler.removeCallbacks(commit)
            mainHandler.postDelayed(commit, settleMs)
        }
    }

    fun start() {
        if (sensorListener.canDetectOrientation()) sensorListener.enable()
    }

    fun stop() {
        sensorListener.disable()
        mainHandler.removeCallbacks(commit)
    }

    /** Registers [listener]; the returned function unregisters it. */
    fun addListener(listener: (Int) -> Unit): () -> Unit {
        listeners += listener
        return { listeners -= listener }
    }
}
