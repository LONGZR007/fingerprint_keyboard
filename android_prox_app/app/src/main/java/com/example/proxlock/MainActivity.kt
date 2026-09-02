package com.example.proxlock

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

class MainActivity : ComponentActivity() {

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { granted ->
        if (granted.values.all { it }) {
            ProxBleService.start(this)
        }
    }

    private val pairLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { /* 系统配对完成后 BLE 重连逻辑自动接管 */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            ProxTheme {
                GuardScreen(
                    onStart = {
                        requestPermissionsThenStart()
                    },
                    onStop = { ProxBleService.stop(this) },
                    onPair = {
                        try {
                            pairLauncher.launch(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
                        } catch (_: Exception) {
                            pairLauncher.launch(Intent(Settings.ACTION_SETTINGS))
                        }
                    }
                )
            }
        }
    }

    private fun requestPermissionsThenStart() {
        val perms = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            perms += Manifest.permission.BLUETOOTH_CONNECT
            perms += Manifest.permission.BLUETOOTH_SCAN
        } else {
            perms += Manifest.permission.ACCESS_FINE_LOCATION
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            perms += Manifest.permission.POST_NOTIFICATIONS
        }
        val missing = perms.filter {
            checkSelfPermission(it) != android.content.pm.PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) {
            ProxBleService.start(this)
        } else {
            permissionLauncher.launch(missing.toTypedArray())
        }
    }
}

/* ============================ 主题与配色 ============================ */

private val BlueLight = Color(0xFF3B82F6)
private val BlueDark = Color(0xFF2563EB)
private val GreenOk = Color(0xFF22C55E)
private val AmberWarn = Color(0xFFF59E0B)
private val RedBad = Color(0xFFEF4444)

private val LightBg = Color(0xFFF8FAFC)
private val LightText = Color(0xFF1E293B)
private val LightSub = Color(0xFF94A3B8)
private val DarkBg = Color(0xFF0F172A)
private val DarkText = Color(0xFFE2E8F0)
private val DarkSub = Color(0xFF94A3B8)

@Composable
private fun ProxTheme(content: @Composable () -> Unit) {
    val dark = androidx.compose.foundation.isSystemInDarkTheme()
    MaterialTheme(
        colorScheme = if (dark)
            darkColorScheme(
                primary = BlueLight,
                background = DarkBg,
                onBackground = DarkText,
                surface = Color(0xFF1E293B),
                onSurface = DarkText
            )
        else
            lightColorScheme(
                primary = BlueDark,
                background = LightBg,
                onBackground = LightText,
                surface = Color.White,
                onSurface = LightText
            ),
        content = content
    )
}

/* ============================ 界面 ============================ */

@Composable
private fun GuardScreen(
    onStart: () -> Unit,
    onStop: () -> Unit,
    onPair: () -> Unit
) {
    val st by BleManager.state.collectAsState()
    val bg = collectBackground()
    val sub = collectSubColor()
    val context = LocalContext.current

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(bg)
            .padding(24.dp),
        verticalArrangement = Arrangement.SpaceBetween
    ) {
        // ---- 状态大卡片 ----
        StatusCard(st, sub)

        // ---- 信号监测卡 ----
        if (st.phase == BleManager.Phase.CONNECTED) {
            SignalCard(st, sub)
        }

        // ---- 控制按钮 ----
        Button(
            onClick = { if (st.phase == BleManager.Phase.STOPPED) onStart() else onStop() },
            modifier = Modifier
                .fillMaxWidth()
                .height(64.dp),
            shape = RoundedCornerShape(32.dp),
            colors = ButtonDefaults.buttonColors(
                containerColor = if (st.phase == BleManager.Phase.STOPPED) BlueDark else RedBad
            )
        ) {
            val label = if (st.phase == BleManager.Phase.STOPPED)
                stringResource(R.string.btn_start)
            else
                stringResource(R.string.btn_stop)
            Text(label, fontSize = 18.sp, fontWeight = FontWeight.SemiBold)
        }

        // ---- 引导区 ----
        Column {
            Text(stringResource(R.string.hint_keepalive), fontSize = 12.sp, color = sub)
            Spacer(Modifier.height(10.dp))
            Text(stringResource(R.string.hint_pair), fontSize = 12.sp, color = sub)
            Spacer(Modifier.height(10.dp))
            OutlinedButton(
                onClick = {
                    try {
                        context.startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
                    } catch (_: Exception) {
                        context.startActivity(Intent(Settings.ACTION_SETTINGS))
                    }
                },
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(16.dp)
            ) {
                Text(stringResource(R.string.btn_pair))
            }
        }
    }
}

@Composable
private fun StatusCard(st: BleManager.UiState, sub: Color) {
    val (title, dotColor) = when (st.phase) {
        BleManager.Phase.CONNECTED -> stringResource(R.string.guard_on) to GreenOk
        BleManager.Phase.RECONNECTING -> stringResource(R.string.guard_reconnecting) to AmberWarn
        BleManager.Phase.SCANNING, BleManager.Phase.CONNECTING ->
            "连接中…" to AmberWarn
        else -> stringResource(R.string.guard_off) to LightSub
    }
    val animatedDot by animateColorAsState(dotColor, tween(300), label = "dot")

    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(
            modifier = Modifier
                .size(72.dp)
                .clip(CircleShape)
                .background(
                    Brush.radialGradient(
                        listOf(animatedDot.copy(alpha = 0.9f), animatedDot.copy(alpha = 0.55f))
                    )
                )
        )
        Spacer(Modifier.width(18.dp))
        Column {
            Text(title, fontSize = 26.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(4.dp))
            Text(
                st.deviceName.ifEmpty { stringResource(R.string.device_default) }
                        + if (st.retryCount > 1) "  (第 ${st.retryCount} 次)" else "",
                fontSize = 14.sp, color = sub
            )
        }
    }
}

@Composable
private fun SignalCard(st: BleManager.UiState, sub: Color) {
    val rssi = st.rssi
    // 距离档位: 近=绿(>-62) 中=黄(-70~-62) 远=红(<-70)
    val barColor = when {
        rssi == null -> LightSub
        rssi > -62 -> GreenOk
        rssi >= -70 -> AmberWarn
        else -> RedBad
    }
    val fill by animateFloatAsState(
        targetValue = ((100 + (rssi ?: -100)).coerceIn(0, 45)) / 45f,
        animationSpec = tween(300), label = "bar"
    )
    val stateText = when (st.proxState) {
        BleManager.PROX_ARMED -> stringResource(R.string.state_armed)
        BleManager.PROX_TRIGGERED -> stringResource(R.string.state_triggered)
        BleManager.PROX_COOLDOWN -> stringResource(R.string.state_cooldown)
        BleManager.PROX_OFF -> stringResource(R.string.state_off)
        else -> stringResource(R.string.state_unknown)
    }

    Surface(shape = RoundedCornerShape(24.dp), tonalElevation = 2.dp) {
        Column(Modifier.fillMaxWidth().padding(20.dp)) {
            Text(
                if (rssi != null) "$rssi" else "--",
                fontSize = 52.sp, fontWeight = FontWeight.Bold
            )
            Text("dBm · 判定: $stateText", fontSize = 13.sp, color = sub)
            Spacer(Modifier.height(14.dp))
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(10.dp)
                    .clip(RoundedCornerShape(5.dp))
                    .background(MaterialTheme.colorScheme.surfaceVariant)
            ) {
                Box(
                    Modifier
                        .fillMaxWidth(fill)
                        .fillMaxHeight()
                        .background(barColor)
                )
            }
            Spacer(Modifier.height(6.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("远", fontSize = 11.sp, color = sub)
                Text("中", fontSize = 11.sp, color = sub)
                Text("近", fontSize = 11.sp, color = sub)
            }
        }
    }
}

/* 收集背景色（深浅色自适应） */
@Composable
private fun collectBackground(): Color {
    return if (androidx.compose.foundation.isSystemInDarkTheme()) DarkBg else LightBg
}

@Composable
private fun collectSubColor(): Color {
    val dark = androidx.compose.foundation.isSystemInDarkTheme()
    return if (dark) DarkSub else LightSub
}
