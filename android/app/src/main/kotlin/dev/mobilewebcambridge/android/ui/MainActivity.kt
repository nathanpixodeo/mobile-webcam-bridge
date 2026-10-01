package dev.mobilewebcambridge.android.ui

import android.Manifest
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import dev.mobilewebcambridge.android.appGraph
import dev.mobilewebcambridge.android.service.StreamingService
import dev.mobilewebcambridge.android.session.ConnectionStatus
import dev.mobilewebcambridge.protocol.messages.PermissionState

class MainActivity : ComponentActivity() {
    private val standby = mutableStateOf(false)
    private var autoStartPending = false

    private val permissionRequest = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { results ->
        val permissions = appGraph.permissions
        permissions.markRequested(results.keys)
        if (permissions.hasCamera || permissions.hasMicrophone) StreamingService.start(this)
    }

    private val actions = object : ScreenActions {
        override fun requestPermissions() = permissionRequest.launch(requiredPermissions())

        override fun openAppSettings() {
            startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", packageName, null)))
        }

        override fun start() = StreamingService.start(this@MainActivity)

        override fun stop() = StreamingService.stop(this@MainActivity)

        override fun enterStandby() = setStandby(true)

        override fun setTestPattern(enabled: Boolean) = appGraph.runtime.setTestPattern(enabled)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        // Open the app = ready to stream: start the server once per launch, not on every resume
        // (the user may have stopped it).
        autoStartPending = savedInstanceState == null
        val graph = appGraph
        setContent {
            MobileWebcamTheme {
                val running by graph.runtime.isRunning.collectAsStateWithLifecycle()
                val session by graph.runtime.snapshot.collectAsStateWithLifecycle()
                val system by graph.systemMonitor.snapshot.collectAsStateWithLifecycle()
                val testPattern by graph.runtime.usesTestPattern.collectAsStateWithLifecycle()
                val logs by graph.logger.recent.collectAsStateWithLifecycle()
                val isStandby by standby
                MobileWebcamScreen(ScreenState(running, session, system, testPattern, logs), actions)
                if (isStandby) {
                    BackHandler { setStandby(false) }
                    StandbyOverlay(connected = session.connection == ConnectionStatus.Connected, onWake = { setStandby(false) })
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        val graph = appGraph
        graph.permissions.refresh()
        if (!autoStartPending) return
        autoStartPending = false
        when {
            graph.permissions.hasCamera || graph.permissions.hasMicrophone -> StreamingService.start(this)
            graph.permissions.status.value.camera == PermissionState.NOT_DETERMINED ->
                permissionRequest.launch(requiredPermissions())
        }
    }

    override fun onPause() {
        super.onPause()
        if (standby.value) setStandby(false) // standby only makes sense while visible
    }

    private fun requiredPermissions(): Array<String> = buildList {
        add(Manifest.permission.CAMERA)
        add(Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) add(Manifest.permission.POST_NOTIFICATIONS)
    }.toTypedArray()

    /**
     * Standby: black overlay, system bars hidden, screen kept on at minimum brightness. On OLED
     * panels black pixels are off, so this costs almost nothing while keeping the app in front.
     */
    private fun setStandby(enabled: Boolean) {
        standby.value = enabled
        val controller = WindowCompat.getInsetsController(window, window.decorView)
        val attributes = window.attributes
        if (enabled) {
            controller.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            controller.hide(WindowInsetsCompat.Type.systemBars())
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            attributes.screenBrightness = STANDBY_BRIGHTNESS
        } else {
            controller.show(WindowInsetsCompat.Type.systemBars())
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            attributes.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        }
        window.attributes = attributes
    }

    private companion object {
        const val STANDBY_BRIGHTNESS = 0.01f
    }
}
