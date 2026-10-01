package dev.mobilewebcambridge.android.system

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import androidx.core.content.edit
import dev.mobilewebcambridge.android.capture.CameraCatalog
import dev.mobilewebcambridge.android.session.DiagnosticsSource
import java.time.Instant

/**
 * Why earlier runs of the app ended, from `ActivityManager.getHistoricalProcessExitReasons`
 * (Android 11+): crashes, ANRs and low-memory kills, each forwarded to the host once. The Android
 * counterpart of the iOS MetricKit reports.
 */
class ExitReasonDiagnostics(private val context: Context) : DiagnosticsSource {
    private val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    @Synchronized
    override fun takePendingReports(limit: Int): List<String> {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return emptyList()
        val activityManager = context.getSystemService(ActivityManager::class.java) ?: return emptyList()
        val lastReported = prefs.getLong(KEY_LAST_REPORTED, 0L)
        val exits = runCatching { activityManager.getHistoricalProcessExitReasons(context.packageName, 0, MAX_EXITS) }
            .getOrDefault(emptyList())
            .filter { it.timestamp > lastReported && it.reason in REPORTED_REASONS }
            .sortedBy { it.timestamp }
            .takeLast(limit)
        if (exits.isEmpty()) return emptyList()
        prefs.edit { putLong(KEY_LAST_REPORTED, exits.last().timestamp) }
        return exits.map { exit ->
            val description = exit.description?.takeIf { it.isNotBlank() }?.let { ": $it" } ?: ""
            "Previous run ended at ${Instant.ofEpochMilli(exit.timestamp)} by ${reasonName(exit.reason)}$description"
        }
    }

    private fun reasonName(reason: Int): String = when (reason) {
        ApplicationExitInfo.REASON_CRASH -> "a crash"
        ApplicationExitInfo.REASON_CRASH_NATIVE -> "a native crash"
        ApplicationExitInfo.REASON_ANR -> "an ANR"
        ApplicationExitInfo.REASON_LOW_MEMORY -> "a low-memory kill"
        ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE -> "excessive resource use"
        ApplicationExitInfo.REASON_INITIALIZATION_FAILURE -> "an initialisation failure"
        else -> "reason $reason"
    }

    private companion object {
        const val PREFS_NAME = "diagnostics"
        const val KEY_LAST_REPORTED = "lastReportedExit"
        const val MAX_EXITS = 16
        val REPORTED_REASONS = setOf(
            ApplicationExitInfo.REASON_CRASH,
            ApplicationExitInfo.REASON_CRASH_NATIVE,
            ApplicationExitInfo.REASON_ANR,
            ApplicationExitInfo.REASON_LOW_MEMORY,
            ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE,
            ApplicationExitInfo.REASON_INITIALIZATION_FAILURE,
        )
    }
}

/** Device facts logged to the host after every handshake ("boot" category). */
object BootReport {
    fun collect(catalog: CameraCatalog): List<String> = buildList {
        add(
            "SDK ${Build.VERSION.SDK_INT}, ${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE}), " +
                "ABI ${Build.SUPPORTED_ABIS.firstOrNull() ?: "unknown"}",
        )
        runCatching { catalog.describe() }
            .onSuccess { cameras ->
                val described = cameras.joinToString("; ") { camera ->
                    buildString {
                        append("${camera.id} ${camera.facing.name.lowercase()} sensor ${camera.sensorOrientation}deg")
                        append(" fov ${"%.2f".format(camera.fieldOfView)}")
                        if (camera.physicalLenses.isNotEmpty()) append(", ${camera.physicalLenses.size} lenses")
                        camera.zoomRatioRange?.let { append(", zoom ${it.start}-${it.endInclusive}") }
                        if (!camera.realtimeTimestamps) append(", monotonic timestamps")
                    }
                }
                add("Cameras: ${described.ifEmpty { "none" }}")
            }
            .onFailure { add("Cameras: unreadable (${it.message})") }
        val encoders = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
            .filter { info -> info.isEncoder && info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_AVC, ignoreCase = true) } }
            .joinToString { info -> if (info.isHardwareAccelerated) "${info.name} (hardware)" else info.name }
        add("H.264 encoders: ${encoders.ifEmpty { "none" }}")
    }
}
