package com.hifiplayer.ui

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import com.hifiplayer.data.PlaybackController
import kotlinx.coroutines.flow.StateFlow

/**
 * ViewModel del player: expone los StateFlows del PlaybackController y sus comandos.
 * El estado incluye PlayerUiState.Suspended (hot-plug) y los avisos de telemetria.
 */
class PlayerViewModel(app: Application) : AndroidViewModel(app) {

    val uiState: StateFlow<PlayerUiState> = PlaybackController.uiState
    val position: StateFlow<Long> = PlaybackController.position
    val duration: StateFlow<Long> = PlaybackController.duration
    val bpWarning: StateFlow<Boolean> = PlaybackController.bpWarning
    val notice: StateFlow<String?> = PlaybackController.notice
    val trackName: StateFlow<String?> = PlaybackController.trackName

    init {
        PlaybackController.init(app)
    }

    fun openList(uris: List<Uri>, startAt: Int = 0) = PlaybackController.openList(uris, startAt)
    fun play() = PlaybackController.play()
    fun pause() = PlaybackController.pause()
    fun stop() = PlaybackController.stop()
    fun seek(ms: Long) = PlaybackController.seek(ms)
    fun next() = PlaybackController.next()
    fun previous() = PlaybackController.previous()
    fun setVolume(v: Float) = PlaybackController.setVolume(v)
    fun setBitPerfect(on: Boolean) = PlaybackController.setBitPerfect(on)
}