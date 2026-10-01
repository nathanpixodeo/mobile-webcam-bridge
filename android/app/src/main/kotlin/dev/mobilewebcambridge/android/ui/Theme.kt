package dev.mobilewebcambridge.android.ui

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

private val Accent = Color(0xFF3D7BFF)

/** Material 3 with the wallpaper colours on Android 12+, the app accent before. */
@Composable
fun MobileWebcamTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val context = LocalContext.current
    val colors = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> if (dark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        dark -> darkColorScheme(primary = Accent)
        else -> lightColorScheme(primary = Accent)
    }
    MaterialTheme(colorScheme = colors, content = content)
}

/** Status colours that read on both light and dark surfaces. */
object StatusColors {
    val good = Color(0xFF2E9E4F)
    val busy = Color(0xFFE08A00)
    val warning = Color(0xFFC9A100)
    val bad = Color(0xFFD93025)
}
