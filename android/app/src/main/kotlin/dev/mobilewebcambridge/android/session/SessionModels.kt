package dev.mobilewebcambridge.android.session

import dev.mobilewebcambridge.protocol.messages.AppState
import dev.mobilewebcambridge.protocol.messages.AudioStreamStatus
import dev.mobilewebcambridge.protocol.messages.BatteryStatus
import dev.mobilewebcambridge.protocol.messages.PermissionState
import dev.mobilewebcambridge.protocol.messages.PermissionsStatus
import dev.mobilewebcambridge.protocol.messages.VideoStreamStatus
import dev.mobilewebcambridge.protocol.policy.ThermalLevel

/** Device facts reported in `Status`, gathered by `SystemStatusMonitor`. */
data class SystemSnapshot(
    val appState: AppState = AppState.ACTIVE,
    val battery: BatteryStatus? = null,
    val lowPower: Boolean = false,
    val permissions: PermissionsStatus = PermissionsStatus(PermissionState.NOT_DETERMINED, PermissionState.NOT_DETERMINED),
    val thermal: ThermalLevel = ThermalLevel.NOMINAL,
)

sealed interface ConnectionStatus {
    data object Stopped : ConnectionStatus

    data object Starting : ConnectionStatus

    data class Listening(val port: Int) : ConnectionStatus

    data class ListenerFailed(val message: String) : ConnectionStatus

    data object Handshaking : ConnectionStatus

    data object Connected : ConnectionStatus
}

/** What the UI shows; published by the session at most twice a second. */
data class SessionSnapshot(
    val connection: ConnectionStatus = ConnectionStatus.Stopped,
    /** The host application from its Hello, e.g. `mobile-webcam-bridge 0.1.0`. */
    val host: String? = null,
    val video: VideoStreamStatus = VideoStreamStatus.OFF,
    val audio: AudioStreamStatus = AudioStreamStatus.OFF,
    val roundTripMs: Int? = null,
    val videoFps: Int = 0,
    val videoKbps: Int = 0,
    val droppedVideoFrames: Int = 0,
)

/** Reports about earlier runs (crashes, ANRs), forwarded to the host once. */
fun interface DiagnosticsSource {
    fun takePendingReports(limit: Int): List<String>
}
