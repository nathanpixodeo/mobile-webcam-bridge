package dev.mobilewebcambridge.android.system

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.PowerManager
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.ProcessLifecycleOwner
import dev.mobilewebcambridge.android.core.AppLogger
import dev.mobilewebcambridge.android.session.SystemSnapshot
import dev.mobilewebcambridge.protocol.messages.AppState
import dev.mobilewebcambridge.protocol.messages.BatteryStatus
import dev.mobilewebcambridge.protocol.policy.ThermalLevel
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * Gathers the device facts reported in `Status`: battery, battery saver, thermal status, app
 * state and permissions. App-wide and main-thread only; started once by the application.
 */
class SystemStatusMonitor(
    private val context: Context,
    private val permissions: PermissionStore,
    private val logger: AppLogger,
) {
    private val powerManager: PowerManager = context.getSystemService(PowerManager::class.java)
    private val state = MutableStateFlow(SystemSnapshot())

    val snapshot: StateFlow<SystemSnapshot> = state.asStateFlow()

    private var battery: BatteryStatus? = null
    private var lowPower = false
    private var thermal = ThermalLevel.NOMINAL
    private var appState = AppState.BACKGROUND
    private var started = false

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                Intent.ACTION_BATTERY_CHANGED -> battery = batteryStatus(intent)
                PowerManager.ACTION_POWER_SAVE_MODE_CHANGED -> lowPower = powerManager.isPowerSaveMode
            }
            publish()
        }
    }

    private val thermalListener = PowerManager.OnThermalStatusChangedListener { status ->
        val level = thermalLevel(status)
        if (level != thermal) {
            logger.info("system", "Thermal status $status: ${level.name.lowercase()}")
            thermal = level
            publish()
        }
    }

    private val lifecycleObserver = LifecycleEventObserver { _, event ->
        appState = when (event) {
            Lifecycle.Event.ON_RESUME -> AppState.ACTIVE
            Lifecycle.Event.ON_START, Lifecycle.Event.ON_PAUSE -> AppState.INACTIVE
            Lifecycle.Event.ON_CREATE, Lifecycle.Event.ON_STOP -> AppState.BACKGROUND
            else -> return@LifecycleEventObserver
        }
        publish()
    }

    fun start() {
        if (started) return
        started = true
        val filter = IntentFilter().apply {
            addAction(Intent.ACTION_BATTERY_CHANGED)
            addAction(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED)
        }
        // Battery changes are sticky: the current state comes back at once.
        ContextCompat.registerReceiver(context, receiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED)?.let {
            battery = batteryStatus(it)
        }
        lowPower = powerManager.isPowerSaveMode
        thermal = thermalLevel(powerManager.currentThermalStatus)
        powerManager.addThermalStatusListener(ContextCompat.getMainExecutor(context), thermalListener)
        ProcessLifecycleOwner.get().lifecycle.addObserver(lifecycleObserver)
        MainScope().launch { permissions.status.collect { publish() } }
        publish()
    }

    private fun publish() {
        state.value = SystemSnapshot(
            appState = appState,
            battery = battery,
            lowPower = lowPower,
            permissions = permissions.status.value,
            thermal = thermal,
        )
    }

    private fun batteryStatus(intent: Intent): BatteryStatus? {
        val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
        val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
        if (level < 0 || scale <= 0) return null
        val status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
        val plugged = intent.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0)
        val charging = plugged != 0 || status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL
        return BatteryStatus(levelPercent = (level * 100 / scale).coerceIn(0, 100), charging = charging)
    }

    /** `PowerManager` thermal status → the throttling levels shared with iOS. */
    private fun thermalLevel(status: Int): ThermalLevel = when (status) {
        PowerManager.THERMAL_STATUS_NONE -> ThermalLevel.NOMINAL
        PowerManager.THERMAL_STATUS_LIGHT -> ThermalLevel.FAIR
        PowerManager.THERMAL_STATUS_MODERATE -> ThermalLevel.SERIOUS
        PowerManager.THERMAL_STATUS_SEVERE -> ThermalLevel.CRITICAL
        else -> ThermalLevel.SHUTDOWN // CRITICAL, EMERGENCY, SHUTDOWN
    }
}
