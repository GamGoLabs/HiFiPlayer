package com.hifiplayer.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import com.hifiplayer.HiFiApp
import com.hifiplayer.domain.Track

class PlayerViewModel(app: Application) : AndroidViewModel(app) {
    private val controller = (app as HiFiApp).controller
    val ui = controller.ui

    fun setQueue(tracks: List<Track>) = controller.setQueue(tracks)
    fun togglePlayPause() = controller.togglePlayPause()
    fun next() = controller.next()
    fun previous() = controller.previous()
    fun seek(ms: Long) = controller.seek(ms)
    fun setBitPerfect(on: Boolean) = controller.setBitPerfect(on)
}