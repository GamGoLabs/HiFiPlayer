package com.hifiplayer

import android.app.Application
import com.hifiplayer.data.PlaybackController

class HiFiApp : Application() {
    lateinit var controller: PlaybackController
        private set

    override fun onCreate() {
        super.onCreate()
        controller = PlaybackController(this)
    }
}
