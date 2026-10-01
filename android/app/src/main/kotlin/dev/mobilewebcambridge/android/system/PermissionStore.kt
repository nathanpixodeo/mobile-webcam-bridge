package dev.mobilewebcambridge.android.system

import android.Manifest
import android.app.admin.DevicePolicyManager
import android.content.Context
import android.content.pm.PackageManager
import androidx.core.content.ContextCompat
import androidx.core.content.edit
import dev.mobilewebcambridge.protocol.messages.PermissionState
import dev.mobilewebcambridge.protocol.messages.PermissionsStatus
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Camera and microphone permission states in protocol terms. Android only says granted or not, so
 * whether the user was ever asked is remembered here to tell `notDetermined` from `denied`; a
 * camera disabled by device policy is `restricted`. Call [refresh] when the app returns to the
 * foreground (permissions can change in Settings meanwhile).
 */
class PermissionStore(private val context: Context) {
    private val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
    private val state = MutableStateFlow(read())

    val status: StateFlow<PermissionsStatus> = state.asStateFlow()

    val hasCamera: Boolean get() = isGranted(Manifest.permission.CAMERA)
    val hasMicrophone: Boolean get() = isGranted(Manifest.permission.RECORD_AUDIO)

    fun refresh() {
        state.value = read()
    }

    /** Records that the user was asked for [permissions] (the answer is read back by [refresh]). */
    fun markRequested(permissions: Collection<String>) {
        prefs.edit { permissions.forEach { putBoolean(it, true) } }
        refresh()
    }

    private fun read() = PermissionsStatus(
        camera = stateOf(Manifest.permission.CAMERA),
        microphone = stateOf(Manifest.permission.RECORD_AUDIO),
    )

    private fun stateOf(permission: String): PermissionState = when {
        isGranted(permission) -> if (permission == Manifest.permission.CAMERA && cameraDisabledByPolicy()) {
            PermissionState.RESTRICTED
        } else {
            PermissionState.AUTHORIZED
        }
        prefs.getBoolean(permission, false) -> PermissionState.DENIED
        else -> PermissionState.NOT_DETERMINED
    }

    private fun isGranted(permission: String): Boolean =
        ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED

    private fun cameraDisabledByPolicy(): Boolean =
        runCatching { context.getSystemService(DevicePolicyManager::class.java)?.getCameraDisabled(null) == true }.getOrDefault(false)

    private companion object {
        const val PREFS_NAME = "permissions"
    }
}
