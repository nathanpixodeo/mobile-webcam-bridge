package dev.mobilewebcambridge.android.ui

import android.os.SystemClock
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.mobilewebcambridge.android.session.ConnectionStatus
import dev.mobilewebcambridge.android.session.SessionSnapshot
import dev.mobilewebcambridge.android.session.SystemSnapshot
import dev.mobilewebcambridge.protocol.logging.LogEntry
import dev.mobilewebcambridge.protocol.messages.LogLevel
import dev.mobilewebcambridge.protocol.messages.PermissionState
import dev.mobilewebcambridge.protocol.messages.StreamState
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Everything the main screen shows. */
data class ScreenState(
    val running: Boolean,
    val session: SessionSnapshot,
    val system: SystemSnapshot,
    val testPattern: Boolean,
    val logs: List<LogEntry>,
) {
    val needsPermissions: Boolean
        get() = system.permissions.camera != PermissionState.AUTHORIZED || system.permissions.microphone != PermissionState.AUTHORIZED

    /** True when Android will not show the permission dialog again: only Settings can grant it. */
    val permissionsDenied: Boolean
        get() = system.permissions.camera == PermissionState.DENIED || system.permissions.microphone == PermissionState.DENIED
}

interface ScreenActions {
    fun requestPermissions()

    fun openAppSettings()

    fun start()

    fun stop()

    fun enterStandby()

    fun setTestPattern(enabled: Boolean)
}

@Composable
fun MobileWebcamScreen(state: ScreenState, actions: ScreenActions) {
    Scaffold { padding ->
        LazyColumn(
            modifier = Modifier.fillMaxSize().padding(padding),
            contentPadding = PaddingValues(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            item {
                Text("Mobile Webcam", style = MaterialTheme.typography.headlineMedium, fontWeight = FontWeight.SemiBold)
            }
            item { ConnectionCard(state) }
            item { StreamsCard(state.session) }
            if (state.needsPermissions) item { PermissionCard(state, actions) }
            if (state.running && state.session.connection !is ConnectionStatus.Connected) item { UsbHelpCard() }
            item { DeviceCard(state.system) }
            item { ControlsCard(state, actions) }
            item { DiagnosticsCard(state, actions) }
            item { LogCard(state.logs) }
        }
    }
}

@Composable
private fun ConnectionCard(state: ScreenState) {
    val connection = state.session.connection
    val (title, subtitle) = when (connection) {
        ConnectionStatus.Stopped -> "Not running" to "Start to let the computer connect over USB."
        ConnectionStatus.Starting -> "Starting…" to "USB port 27100"
        is ConnectionStatus.Listening ->
            "Waiting for the computer" to "Plug in the USB cable and start the bridge on the PC (port ${connection.port})."
        is ConnectionStatus.ListenerFailed -> "Cannot listen for the computer" to connection.message
        ConnectionStatus.Handshaking -> "Computer connecting…" to "USB port 27100"
        ConnectionStatus.Connected ->
            "Connected to the computer" to (state.session.host?.let { "$it. " } ?: "") +
                "Streams start when an app opens the camera or microphone."
    }
    val color = when (connection) {
        ConnectionStatus.Connected -> StatusColors.good
        is ConnectionStatus.ListenerFailed -> StatusColors.bad
        ConnectionStatus.Starting, ConnectionStatus.Handshaking -> StatusColors.busy
        else -> MaterialTheme.colorScheme.outline
    }
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(modifier = Modifier.size(12.dp).background(color, CircleShape))
            Spacer(Modifier.width(12.dp))
            Column {
                Text(title, style = MaterialTheme.typography.titleMedium)
                Text(subtitle, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

@Composable
private fun StreamsCard(session: SessionSnapshot) {
    val video = session.video
    val videoDetail = video.reason ?: run {
        val width = video.width
        val height = video.height
        val fps = video.fps
        if (width != null && height != null && fps != null) "$width×$height @ $fps fps" else null
    }
    SectionCard("Streams") {
        StreamRow("Camera", video.state, videoDetail)
        StreamRow("Microphone", session.audio.state, session.audio.reason)
    }
}

@Composable
private fun StreamRow(title: String, state: StreamState, detail: String?) {
    val color = when (state) {
        StreamState.RUNNING -> StatusColors.good
        StreamState.STARTING -> StatusColors.busy
        StreamState.INTERRUPTED -> StatusColors.warning
        StreamState.ERROR -> StatusColors.bad
        StreamState.OFF -> MaterialTheme.colorScheme.onSurfaceVariant
    }
    Row(modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(title, modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyLarge)
        Column(horizontalAlignment = Alignment.End) {
            Text(state.wire.replaceFirstChar { it.uppercase() }, color = color, style = MaterialTheme.typography.bodyLarge)
            if (detail != null) {
                Text(detail, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

@Composable
private fun PermissionCard(state: ScreenState, actions: ScreenActions) {
    SectionCard("Camera and microphone access") {
        Text(
            "Mobile Webcam needs the camera and the microphone to stream them to the computer.",
            style = MaterialTheme.typography.bodyMedium,
        )
        Spacer(Modifier.height(8.dp))
        if (state.permissionsDenied) {
            Text(
                "Access was denied. Allow it in Settings › Apps › Mobile Webcam › Permissions.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(8.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = actions::openAppSettings) { Text("Open settings") }
                TextButton(onClick = actions::requestPermissions) { Text("Ask again") }
            }
        } else {
            Button(onClick = actions::requestPermissions) { Text("Allow camera and microphone") }
        }
    }
}

@Composable
private fun UsbHelpCard() {
    SectionCard("Connect over USB") {
        val steps = listOf(
            "Settings › About phone: tap Build number seven times to turn on Developer options.",
            "Developer options (under Settings › System on most phones): turn on USB debugging.",
            "Connect the USB cable, unlock the phone and allow USB debugging for this computer.",
            "On the computer, start the bridge: node src/main.ts start",
        )
        steps.forEachIndexed { index, step ->
            Row(modifier = Modifier.padding(vertical = 2.dp)) {
                Text("${index + 1}.", modifier = Modifier.width(20.dp), style = MaterialTheme.typography.bodyMedium)
                Text(step, style = MaterialTheme.typography.bodyMedium)
            }
        }
        Spacer(Modifier.height(4.dp))
        Text(
            "Menu names vary by manufacturer. Keep the phone unlocked the first time so the prompt can appear.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun DeviceCard(system: SystemSnapshot) {
    SectionCard("Device") {
        LabeledValue("Thermal", system.thermal.statusName.wire.replaceFirstChar { it.uppercase() })
        val battery = system.battery
        LabeledValue("Battery", if (battery == null) "Unknown" else "${battery.levelPercent} %" + if (battery.charging) " (charging)" else "")
        if (system.lowPower) {
            Text("Battery saver is on", color = StatusColors.busy, style = MaterialTheme.typography.bodyMedium)
        }
    }
}

@Composable
private fun ControlsCard(state: ScreenState, actions: ScreenActions) {
    SectionCard("Streaming") {
        Text(
            "Streaming continues with the screen off or another app open. Standby blacks out the screen " +
                "and keeps it on at minimum brightness; tap anywhere to wake it.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            if (state.running) {
                OutlinedButton(onClick = actions::stop) { Text("Stop") }
                Button(onClick = actions::enterStandby) { Text("Standby") }
            } else {
                Button(onClick = actions::start, enabled = hasAnyPermission(state.system)) { Text("Start") }
            }
        }
    }
}

private fun hasAnyPermission(system: SystemSnapshot): Boolean =
    system.permissions.camera == PermissionState.AUTHORIZED || system.permissions.microphone == PermissionState.AUTHORIZED

@Composable
private fun DiagnosticsCard(state: ScreenState, actions: ScreenActions) {
    val session = state.session
    SectionCard("Diagnostics") {
        LabeledValue("Round trip", session.roundTripMs?.let { "$it ms" } ?: "–")
        LabeledValue("Video sent", "${session.videoFps} fps, ${session.videoKbps} kbps")
        LabeledValue("Frames dropped", session.droppedVideoFrames.toString())
        HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text("Test pattern", style = MaterialTheme.typography.bodyLarge)
                Text(
                    "Send colour bars and a 440 Hz tone instead of the camera and microphone.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Switch(checked = state.testPattern, onCheckedChange = actions::setTestPattern)
        }
    }
}

@Composable
private fun LogCard(logs: List<LogEntry>) {
    val format = remember { SimpleDateFormat("HH:mm:ss.SSS", Locale.ROOT) }
    SectionCard("Log") {
        if (logs.isEmpty()) {
            Text("No entries yet", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        // Entries carry device-monotonic time; shown as wall-clock time for readability.
        val wallOffsetMs = System.currentTimeMillis() - SystemClock.elapsedRealtime()
        logs.takeLast(LOG_LINES).asReversed().forEach { entry ->
            val time = format.format(Date(wallOffsetMs + entry.timestampUs / 1_000))
            Text(
                "$time ${entry.level.wire.first().uppercaseChar()} [${entry.category}] ${entry.message}",
                fontFamily = FontFamily.Monospace,
                fontSize = 11.sp,
                lineHeight = 14.sp,
                color = when (entry.level) {
                    LogLevel.ERROR -> StatusColors.bad
                    LogLevel.WARN -> StatusColors.busy
                    else -> MaterialTheme.colorScheme.onSurface
                },
            )
        }
    }
}

@Composable
private fun SectionCard(title: String, content: @Composable () -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(title, style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.height(8.dp))
            content()
        }
    }
}

@Composable
private fun LabeledValue(label: String, value: String) {
    Row(modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
        Text(label, modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
        Text(value, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

/**
 * Full-screen black overlay: on OLED panels black pixels are off, which saves power and heat
 * during long calls. Capture keeps running underneath. Tap to wake.
 */
@Composable
fun StandbyOverlay(connected: Boolean, onWake: () -> Unit) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null, onClick = onWake),
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Box(modifier = Modifier.size(8.dp).background(if (connected) StatusColors.good else Color.Gray, CircleShape))
            Spacer(Modifier.height(8.dp))
            Text("Streaming — tap to wake", color = Color(0xFF404040), fontSize = 13.sp)
        }
    }
}

private const val LOG_LINES = 60
