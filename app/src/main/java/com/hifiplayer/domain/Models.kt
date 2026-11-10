package com.hifiplayer.domain

import android.net.Uri

data class Track(val uri: Uri, val title: String)

enum class EngineState {
    IDLE, READY, PLAYING, PAUSED, ENDED, ERROR, SUSPENDED;

    companion object {
        fun from(i: Int) = entries.getOrElse(i) { IDLE }
    }
}

data class PlayerUiState(
    val title: String = "",
    val state: EngineState = EngineState.IDLE,
    val positionMs: Long = 0,
    val durationMs: Long = 0,
    val index: Int = -1,
    val queueSize: Int = 0,
    val streamInfo: String = "",
    val bitPerfectInfo: String = "",
    val bitPerfectEnabled: Boolean = true,
    // ═══ NUEVOS CAMPOS PARA HOT-PLUG ═══
    val bitPerfectWarning: Boolean = false,    // true si telemetría contiene "WARNING: exclusive mode unavailable"
    val recoveryNotice: String? = null,        // "DAC no reconectado..." si recover=timeout, null si no
)