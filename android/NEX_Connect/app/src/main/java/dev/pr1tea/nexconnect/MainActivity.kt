package dev.pr1tea.nexconnect

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas as AndroidCanvas
import android.graphics.Matrix
import android.graphics.Path
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.Uri
import android.net.wifi.WifiNetworkSpecifier
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.pr1tea.nexconnect.ui.theme.NexTheme
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL
import kotlin.math.cos
import kotlin.math.sin
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private const val SSID = "NEX-Watch"
private const val PASSWORD = "nexwatch1234"
private const val UPLOAD_URL = "http://192.168.4.1/upload"

private enum class Screen { Home, Main, Options, Wallpaper }

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            val prefs = remember { getSharedPreferences("nex", MODE_PRIVATE) }
            var themeMode by remember { mutableStateOf(prefs.getString("theme", "system") ?: "system") }
            val dark = when (themeMode) {
                "dark" -> true
                "light" -> false
                else -> androidx.compose.foundation.isSystemInDarkTheme()
            }
            NexTheme(darkTheme = dark) {
                NexApp(themeMode = themeMode, onThemeChange = {
                    themeMode = it
                    prefs.edit().putString("theme", it).apply()
                })
            }
        }
    }
}

@Composable
private fun NexApp(themeMode: String, onThemeChange: (String) -> Unit) {
    val context = LocalContext.current
    val manager = remember { context.getSystemService(ConnectivityManager::class.java) }
    var screen by remember { mutableStateOf(Screen.Home) }
    var connected by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("未连接") }
    var connecting by remember { mutableStateOf(false) }
    var callback by remember { mutableStateOf<ConnectivityManager.NetworkCallback?>(null) }

    fun connect() {
        if (connecting) return
        connecting = true
        status = "正在连接 NEX-Watch…"
        val specifier = WifiNetworkSpecifier.Builder()
            .setSsid(SSID)
            .setWpa2Passphrase(PASSWORD)
            .build()
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .setNetworkSpecifier(specifier)
            .build()
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                manager.bindProcessToNetwork(network)
                connecting = false
                connected = true
                status = "已连接"
                screen = Screen.Main
            }

            override fun onUnavailable() {
                connecting = false
                status = "连接失败，请确认手表已打开“手机连接”页"
            }

            override fun onLost(network: Network) {
                connected = false
                status = "连接已断开"
            }
        }
        callback = cb
        runCatching { manager.requestNetwork(request, cb, 30000) }.onFailure {
            connecting = false
            status = "无法发起连接：${it.message}"
        }
    }

    Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        when (screen) {
            Screen.Home -> HomeScreen(status, connecting, ::connect)
            Screen.Main -> MainScreen(
                connected = connected,
                status = status,
                onWallpaper = { screen = Screen.Wallpaper },
                onOptions = { screen = Screen.Options },
                onConnect = ::connect,
            )
            Screen.Options -> OptionsScreen(themeMode, onThemeChange) { screen = Screen.Main }
            Screen.Wallpaper -> WallpaperScreen(onBack = { screen = Screen.Main })
        }
    }
}

@Composable
private fun DeviceArt(size: Dp, accent: Color = Color(0xFFFFD24A)) {
    Canvas(modifier = Modifier.size(size)) {
        val center = Offset(this.size.width / 2f, this.size.height / 2f)
        val radius = this.size.minDimension * 0.42f
        drawCircle(Color(0xFF141416), radius, center)
        drawCircle(accent, radius, center, style = Stroke(width = 3.dp.toPx()))
        for (i in 0 until 12) {
            val angle = Math.toRadians(i * 30.0 - 90.0)
            val inner = radius * 0.86f
            val outer = radius * 0.96f
            drawLine(
                color = if (i % 3 == 0) accent else Color(0xFF66666E),
                start = Offset(center.x + cos(angle).toFloat() * inner, center.y + sin(angle).toFloat() * inner),
                end = Offset(center.x + cos(angle).toFloat() * outer, center.y + sin(angle).toFloat() * outer),
                strokeWidth = if (i % 3 == 0) 3f else 1.5f,
            )
        }
        val hour = Math.toRadians(-60.0)
        val minute = Math.toRadians(120.0)
        drawLine(
            Color.White,
            center,
            Offset(center.x + cos(hour).toFloat() * radius * 0.5f, center.y + sin(hour).toFloat() * radius * 0.5f),
            strokeWidth = 6.dp.toPx(),
        )
        drawLine(
            Color.White,
            center,
            Offset(center.x + cos(minute).toFloat() * radius * 0.72f, center.y + sin(minute).toFloat() * radius * 0.72f),
            strokeWidth = 4.dp.toPx(),
        )
        drawCircle(accent, radius * 0.05f, center)
        drawRoundRect(
            color = Color(0xFF2A2A2E),
            topLeft = Offset(center.x - radius * 0.5f, 0f),
            size = androidx.compose.ui.geometry.Size(radius, this.size.height * 0.06f),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(radius * 0.1f),
        )
    }
}

@Composable
private fun HomeScreen(status: String, connecting: Boolean, onConnect: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(32.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        DeviceArt(220.dp)
        Spacer(Modifier.height(28.dp))
        Text("NEX", fontSize = 54.sp, fontWeight = FontWeight.Black, letterSpacing = 6.sp)
        Text(
            "M5Stack Stopwatch",
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(48.dp))
        Button(
            onClick = onConnect,
            enabled = !connecting,
            shape = RoundedCornerShape(36.dp),
            modifier = Modifier.fillMaxWidth().height(68.dp),
        ) {
            Text(if (connecting) "正在连接…" else "连接设备", fontSize = 18.sp, fontWeight = FontWeight.Bold)
        }
        Spacer(Modifier.height(16.dp))
        Text(status, color = MaterialTheme.colorScheme.onSurfaceVariant, textAlign = TextAlign.Center)
        Spacer(Modifier.height(8.dp))
        Text(
            "先在手表上进入：设置 → 手机连接",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun MainScreen(
    connected: Boolean,
    status: String,
    onWallpaper: () -> Unit,
    onOptions: () -> Unit,
    onConnect: () -> Unit,
) {
    Scaffold(
        bottomBar = {
            NavigationBar {
                NavigationBarItem(
                    selected = true,
                    onClick = {},
                    icon = { Box(Modifier.size(22.dp).clip(CircleShape).background(MaterialTheme.colorScheme.primary)) },
                    label = { Text("我的 M5Stack Stopwatch", fontSize = 11.sp) },
                )
                NavigationBarItem(
                    selected = false,
                    onClick = onOptions,
                    icon = { Box(Modifier.size(22.dp).clip(RoundedCornerShape(8.dp)).background(MaterialTheme.colorScheme.secondary)) },
                    label = { Text("应用选项", fontSize = 11.sp) },
                )
            }
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Spacer(Modifier.height(24.dp))
            Text("NEX", fontSize = 30.sp, fontWeight = FontWeight.Black)
            Text(
                if (connected) "已连接 NEX-Watch" else status,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(12.dp))
            DeviceArt(180.dp)
            Spacer(Modifier.height(12.dp))
            if (!connected) {
                TextButton(onClick = onConnect) { Text("重新连接设备") }
            }
            Spacer(Modifier.height(20.dp))
            Card(
                shape = RoundedCornerShape(32.dp),
                modifier = Modifier.fillMaxWidth().clickable { onWallpaper() },
            ) {
                Column(modifier = Modifier.padding(24.dp)) {
                    Text("更换壁纸", fontSize = 22.sp, fontWeight = FontWeight.Bold)
                    Spacer(Modifier.height(6.dp))
                    Text(
                        "从相册选择图片，圆形裁剪后上传到手表",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

@Composable
private fun OptionsScreen(themeMode: String, onThemeChange: (String) -> Unit, onBack: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(24.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onBack) { Text("返回") }
            Spacer(Modifier.width(8.dp))
            Text("应用选项", fontSize = 28.sp, fontWeight = FontWeight.Bold)
        }
        Spacer(Modifier.height(16.dp))
        Text("外观", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold)
        listOf(
            "system" to "跟随系统",
            "light" to "浅色",
            "dark" to "深色",
        ).forEach { (value, label) ->
            Row(
                modifier = Modifier.fillMaxWidth().clickable { onThemeChange(value) }.padding(vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                RadioButton(selected = themeMode == value, onClick = { onThemeChange(value) })
                Text(label, fontSize = 18.sp)
            }
        }
        Spacer(Modifier.height(24.dp))
        HorizontalDivider()
        Spacer(Modifier.height(24.dp))
        Text("关于", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold)
        Spacer(Modifier.height(12.dp))
        AboutText()
    }
}

@Composable
private fun AboutText() {
    val lines = listOf(
        "开发者：PR1TEA",
        "包名：dev.pr1tea.nexconnect",
        "许可证：GNU General Public License v3.0",
        "",
        "本程序为自由软件，按 GPL-3.0 发布，不提供任何担保；你可以再分发和/或修改它，但需遵守同一许可证。",
        "",
        "本应用是独立项目，与 M5Stack 无附属关系；M5Stack 是其权利人的商标。",
        "",
        "使用 Android、Jetpack Compose 等开源工具构建，相关版权归各自权利人所有。",
    )
    Text(
        lines.joinToString("\n"),
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

@Composable
private fun WallpaperScreen(onBack: () -> Unit) {
    val context = LocalContext.current
    val density = androidx.compose.ui.platform.LocalDensity.current
    val scope = rememberCoroutineScope()
    var bitmap by remember { mutableStateOf<Bitmap?>(null) }
    var scale by remember { mutableStateOf(1f) }
    var offset by remember { mutableStateOf(Offset.Zero) }
    var busy by remember { mutableStateOf(false) }
    var message by remember { mutableStateOf("选择一张图片，拖动/双指缩放") }

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri: Uri? ->
        bitmap = uri?.let { loadBitmap(context, it) }
        scale = 1f
        offset = Offset.Zero
        message = if (bitmap == null) "无法读取图片" else "拖动/双指缩放，圆形区域将上传"
    }

    Column(
        modifier = Modifier.fillMaxSize().padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onBack) { Text("返回") }
            Spacer(Modifier.width(8.dp))
            Text("更换壁纸", fontSize = 28.sp, fontWeight = FontWeight.Bold)
        }
        Spacer(Modifier.height(16.dp))
        Box(
            modifier = Modifier
                .size(300.dp)
                .clip(CircleShape)
                .background(MaterialTheme.colorScheme.surfaceVariant)
                .pointerInput(Unit) {
                    detectTransformGestures { _, pan, zoom, _ ->
                        scale = (scale * zoom).coerceIn(1f, 5f)
                        offset += pan
                    }
                },
            contentAlignment = Alignment.Center,
        ) {
            val current = bitmap
            if (current != null) {
                Image(
                    bitmap = current.asImageBitmap(),
                    contentDescription = null,
                    contentScale = ContentScale.Crop,
                    modifier = Modifier
                        .fillMaxSize()
                        .graphicsLayer(
                            scaleX = scale,
                            scaleY = scale,
                            translationX = offset.x,
                            translationY = offset.y,
                        ),
                )
            } else {
                Text("未选择图片", color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        Spacer(Modifier.height(16.dp))
        Button(
            onClick = {
                picker.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
            },
            shape = RoundedCornerShape(28.dp),
        ) { Text(if (bitmap == null) "选择图片" else "重新选择") }
        Spacer(Modifier.height(12.dp))
        Button(
            enabled = bitmap != null && !busy,
            shape = RoundedCornerShape(28.dp),
            modifier = Modifier.fillMaxWidth().height(60.dp),
            onClick = {
                val source = bitmap ?: return@Button
                busy = true
                message = "正在处理并上传…"
                scope.launch {
                    val boxPx = with(density) { 300.dp.toPx() }
                    val bytes = withContext(Dispatchers.Default) {
                        renderJpeg(source, scale, offset, boxPx)
                    }
                    message = if (bytes == null) {
                        "图片处理失败"
                    } else if (uploadJpeg(bytes)) {
                        "上传成功，手表已更新壁纸"
                    } else {
                        "上传失败，请确认已连接 NEX-Watch"
                    }
                    busy = false
                }
            },
        ) { Text(if (busy) "上传中…" else "确认并上传", fontSize = 18.sp) }
        Spacer(Modifier.height(10.dp))
        Text(message, textAlign = TextAlign.Center, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

private fun loadBitmap(context: android.content.Context, uri: Uri): Bitmap? =
    runCatching {
        context.contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it) }
    }.getOrNull()

private fun renderJpeg(source: Bitmap, userScale: Float, offset: Offset, boxPx: Float): ByteArray? =
    runCatching {
        val output = Bitmap.createBitmap(466, 466, Bitmap.Config.ARGB_8888)
        val canvas = AndroidCanvas(output)
        canvas.drawColor(android.graphics.Color.BLACK)
        val clip = Path().apply { addCircle(233f, 233f, 233f, Path.Direction.CW) }
        canvas.clipPath(clip)
        val base = maxOf(boxPx / source.width, boxPx / source.height)
        val factor = 466f / boxPx
        val total = base * userScale * factor
        val matrix = Matrix()
        matrix.postScale(total, total)
        matrix.postTranslate(
            233f - source.width * total / 2f + offset.x * factor,
            233f - source.height * total / 2f + offset.y * factor,
        )
        canvas.drawBitmap(source, matrix, null)
        val stream = ByteArrayOutputStream()
        output.compress(Bitmap.CompressFormat.JPEG, 85, stream)
        output.recycle()
        stream.toByteArray()
    }.getOrNull()

private suspend fun uploadJpeg(bytes: ByteArray): Boolean = withContext(Dispatchers.IO) {
    runCatching {
        val connection = URL(UPLOAD_URL).openConnection() as HttpURLConnection
        connection.requestMethod = "POST"
        connection.doOutput = true
        connection.connectTimeout = 10000
        connection.readTimeout = 20000
        connection.setRequestProperty("Content-Type", "image/jpeg")
        connection.outputStream.use { it.write(bytes) }
        val code = connection.responseCode
        connection.disconnect()
        code in 200..299
    }.getOrDefault(false)
}
