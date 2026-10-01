package dev.mobilewebcambridge.android.core

import android.content.Context
import android.os.Build
import android.provider.Settings
import dev.mobilewebcambridge.android.BuildConfig
import dev.mobilewebcambridge.protocol.messages.AppDescriptor
import dev.mobilewebcambridge.protocol.messages.DeviceDescriptor
import dev.mobilewebcambridge.protocol.messages.HelloMessage
import dev.mobilewebcambridge.protocol.messages.PeerRole

/** Facts about this build and device, gathered once and passed around as a value. */
data class AppIdentity(val app: AppDescriptor, val device: DeviceDescriptor) {
    val hello: HelloMessage
        get() = HelloMessage(role = PeerRole.DEVICE, app = app, device = device, features = FEATURES)

    companion object {
        const val APP_NAME: String = "mobile-webcam-bridge-android"
        val FEATURES: List<String> = listOf("audio.pcm", "log", "status", "video.h264")

        fun current(context: Context): AppIdentity {
            val deviceName = Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME) ?: Build.MODEL
            return AppIdentity(
                app = AppDescriptor(
                    name = APP_NAME,
                    version = BuildConfig.VERSION_NAME,
                    build = BuildConfig.VERSION_CODE.toString(),
                    gitSha = BuildConfig.GIT_SHA,
                ),
                device = DeviceDescriptor(model = Build.MODEL, name = deviceName, os = "Android ${Build.VERSION.RELEASE}"),
            )
        }
    }
}
