package com.hifiplayer.ui

import android.Manifest
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.OpenableColumns
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.hifiplayer.domain.EngineState
import com.hifiplayer.domain.Track
import com.hifiplayer.service.PlaybackService

class MainActivity : ComponentActivity() {
    private val vm: PlayerViewModel by viewModels()

    private val notifPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= 33) notifPermission.launch(Manifest.permission.POST_NOTIFICATIONS)

        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                Surface(Modifier.fillMaxSize()) {
                    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
                        if (uris.isEmpty()) return@rememberLauncherForActivityResult
                        uris.forEach {
                            runCatching { contentResolver.takePersistableUriPermission(it, Intent.FLAG_GRANT_READ_URI_PERMISSION) }
                        }
                        ContextCompat.startForegroundService(this, Intent(this, PlaybackService::class.java))
                        vm.setQueue(uris.map { Track(it, displayName(it)) })
                    }
                    PlayerScreen(
                        vm,
                        onPick = { picker.launch(arrayOf("audio/flac", "audio/x-flac", "audio/wav", "audio/x-wav", "audio/*")) },
                        onBattery = ::requestIgnoreBatteryOptimizations,
                    )
                }
            }
        }
    }

    private fun displayName(uri: Uri): String =
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
            if (it.moveToFirst()) it.getString(0) else null
        } ?: uri.lastPathSegment.orEmpty()

    private fun requestIgnoreBatteryOptimizations() {
        val pm = getSystemService(PowerManager::class.java)
        if (!pm.isIgnoringBatteryOptimizations(packageName))
            startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName")))
    }
}

@Composable
fun PlayerScreen(vm: PlayerViewModel, onPick: () -> Unit, onBattery: () -> Unit) {
    val ui by vm.ui.collectAsStateWithLifecycle()
    var dragging by remember { mutableStateOf<Float?>(null) }
    val progress = if (ui.durationMs > 0) ui.positionMs.toFloat() / ui.durationMs else 0f

    Column(Modifier.fillMaxSize().padding(24.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("HiFi Player — Fase 1", style = MaterialTheme.typography.titleLarge)
        Button(onClick = onPick) { Text("Elegir archivos FLAC/WAV") }
        OutlinedButton(onClick = onBattery) { Text("Excluir de optimización de batería (Doze)") }

        Spacer(Modifier.height(8.dp))
        Text(ui.title.ifEmpty { "—" }, style = MaterialTheme.typography.titleMedium)
        Text("Pista ${ui.index + 1}/${ui.queueSize}  ·  ${ui.state}")

        // ═══════════════════════════════════════════════════════════════
        // PATCH HOT-PLUG: banners de estado (Suspended + warnings)
        // ═══════════════════════════════════════════════════════════════
        // Banner de suspensión (DAC desconectado esperando reconexión)
        if (ui.state == EngineState.SUSPENDED) {
            Text(
                text = "⏳ Esperando DAC… reanudación automática al reconectar",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.tertiary,
                textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth()
            )
        }
        // Aviso de bit-perfect denegado (modo compartido forzado)
        if (ui.bitPerfectWarning) {
            Text(
                text = "⚠ Modo compartido: sin bit-perfect con este DAC/ajuste",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error
            )
        }
        // Aviso de recovery timeout (DAC no reconectó a tiempo)
        ui.recoveryNotice?.let { notice ->
            Text(
                text = notice,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error
            )
        }
        // ═══════════════════════════════════════════════════════════════

        Slider(
            value = dragging ?: progress,
            onValueChange = { dragging = it },
            onValueChangeFinished = { dragging?.let { vm.seek((it * ui.durationMs).toLong()) }; dragging = null },
        )
        Text("${fmt(ui.positionMs)} / ${fmt(ui.durationMs)}")

        Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Button(onClick = vm::previous) { Text("⏮") }
            Button(onClick = vm::togglePlayPause) { Text(if (ui.state == EngineState.PLAYING) "⏸" else "▶") }
            Button(onClick = vm::next) { Text("⏭") }
        }

        Row(verticalAlignment = Alignment.CenterVertically) {
            Switch(checked = ui.bitPerfectEnabled, onCheckedChange = vm::setBitPerfect)
            Spacer(Modifier.width(8.dp)); Text("Salida bit-perfect (I16/I32 nativo)")
        }
        Text(ui.streamInfo, style = MaterialTheme.typography.bodySmall)
        Text(ui.bitPerfectInfo, style = MaterialTheme.typography.bodySmall)
    }
}

private fun fmt(ms: Long): String { val s = ms / 1000; return "%d:%02d".format(s / 60, s % 60) }