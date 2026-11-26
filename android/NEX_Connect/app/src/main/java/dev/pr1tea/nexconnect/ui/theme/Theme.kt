package dev.pr1tea.nexconnect.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val NexDark = darkColorScheme(
    primary = Color(0xFFFFD24A),
    onPrimary = Color(0xFF1A1A1A),
    secondary = Color(0xFF9CCBFF),
    background = Color(0xFF0B0B0D),
    onBackground = Color(0xFFEDEDED),
    surface = Color(0xFF151517),
    onSurface = Color(0xFFEDEDED),
    surfaceVariant = Color(0xFF232326),
    onSurfaceVariant = Color(0xFFB8B8BC),
    outline = Color(0xFF4A4A50),
)

private val NexLight = lightColorScheme(
    primary = Color(0xFF3A5BD9),
    onPrimary = Color(0xFFFFFFFF),
    secondary = Color(0xFF2E6FD6),
    background = Color(0xFFF7F5EF),
    onBackground = Color(0xFF17171A),
    surface = Color(0xFFFFFFFF),
    onSurface = Color(0xFF17171A),
    surfaceVariant = Color(0xFFEDEAE2),
    onSurfaceVariant = Color(0xFF56565E),
    outline = Color(0xFFB9B5AC),
)

@Composable
fun NexTheme(darkTheme: Boolean = isSystemInDarkTheme(), content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = if (darkTheme) NexDark else NexLight,
        typography = Typography(),
        content = content,
    )
}
