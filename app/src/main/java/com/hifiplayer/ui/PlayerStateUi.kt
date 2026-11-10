package com.hifiplayer.ui

/**
 * Estados de UI derivados del estado nativo del motor (AudioEngine::State).
 * Incluye el estado de hot-plug de la Fase 7-parcial: Suspended = 6.
 */
sealed class PlayerUiState {
    object Idle : PlayerUiState()
    object Ready : PlayerUiState()
    object Playing : PlayerUiState()
    object Paused : PlayerUiState()
    object Ended : PlayerUiState()
    object Error : PlayerUiState()
    /** DAC desconectado: el motor reintenta solo (800 ms/intento, timeout 20 s). */
    object Suspended : PlayerUiState()
}

object PlayerStateMapper {
    /** Mapea el int nativo (AudioEngine::State, incluido Suspended=6) a estado de UI. */
    fun from(nativeState: Int): PlayerUiState = when (nativeState) {
        0 -> PlayerUiState.Idle
        1 -> PlayerUiState.Ready
        2 -> PlayerUiState.Playing
        3 -> PlayerUiState.Paused
        4 -> PlayerUiState.Ended
        6 -> PlayerUiState.Suspended
        else -> PlayerUiState.Error
    }

    /** true si la telemetria indica que NO se obtuvo bit-perfect real (compartido forzado). */
    fun bitPerfectWarning(telemetry: String): Boolean =
        telemetry.contains("WARNING: exclusive mode unavailable")

    /** true si la telemetria registra una suspension por hot-plug. */
    fun suspendedByHotplug(telemetry: String): Boolean = telemetry.contains("suspend=1")

    /** true si la ultima recuperacion termino en timeout (quedo en pausa con posicion). */
    fun recoverTimeout(telemetry: String): Boolean = telemetry.contains("recover=timeout")
}