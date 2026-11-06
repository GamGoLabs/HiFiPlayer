package com.hifiplayer.service

import android.app.*
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.ServiceCompat
import com.hifiplayer.HiFiApp
import com.hifiplayer.domain.EngineState
import com.hifiplayer.domain.PlayerUiState
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.collect

/** Foreground service: mantiene el proceso vivo (Doze), expone MediaSession y notificación multimedia. */
class PlaybackService : Service() {
    private lateinit var session: MediaSession
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var wakeLock: PowerManager.WakeLock? = null
    private val controller get() = (application as HiFiApp).controller

    override fun onCreate() {
        super.onCreate()
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL, "Reproducción", NotificationManager.IMPORTANCE_LOW))

        session = MediaSession(this, "HiFiSession").apply {
            setCallback(object : MediaSession.Callback() {
                override fun onPlay() { this@PlaybackService.controller.play() }
                override fun onPause() { this@PlaybackService.controller.pause() }
                override fun onSkipToNext() { this@PlaybackService.controller.next() }
                override fun onSkipToPrevious() { this@PlaybackService.controller.previous() }
                override fun onSeekTo(pos: Long) { this@PlaybackService.controller.seek(pos) }
            })
            isActive = true
        }
        ServiceCompat.startForeground(this, NOTIF_ID, buildNotification(controller.ui.value),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)

        val pm = getSystemService(PowerManager::class.java)
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "HiFi:playback").apply { setReferenceCounted(false) }

        scope.launch {
            var lastKey = ""
            controller.ui.collect { s ->
                if (s.state == EngineState.PLAYING) { if (wakeLock?.isHeld == false) wakeLock?.acquire() }
                else if (wakeLock?.isHeld == true) wakeLock?.release()
                updateSession(s)
                val key = "${s.title}|${s.state}"
                if (key != lastKey) {   // no refrescar la notificación cada 250 ms
                    lastKey = key
                    getSystemService(NotificationManager::class.java).notify(NOTIF_ID, buildNotification(s))
                }
            }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_TOGGLE -> controller.togglePlayPause()
            ACTION_NEXT -> controller.next()
            ACTION_PREV -> controller.previous()
        }
        return START_NOT_STICKY
    }

    private fun updateSession(s: PlayerUiState) {
        val st = when (s.state) {
            EngineState.PLAYING -> PlaybackState.STATE_PLAYING
            EngineState.PAUSED, EngineState.READY -> PlaybackState.STATE_PAUSED
            EngineState.ERROR -> PlaybackState.STATE_ERROR
            else -> PlaybackState.STATE_STOPPED
        }
        session.setPlaybackState(PlaybackState.Builder()
            .setActions(PlaybackState.ACTION_PLAY or PlaybackState.ACTION_PAUSE or PlaybackState.ACTION_PLAY_PAUSE or
                PlaybackState.ACTION_SKIP_TO_NEXT or PlaybackState.ACTION_SKIP_TO_PREVIOUS or PlaybackState.ACTION_SEEK_TO)
            .setState(st, s.positionMs, 1f).build())
    }

    private fun pi(action: String) = PendingIntent.getService(this, action.hashCode(),
        Intent(this, PlaybackService::class.java).setAction(action),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

    private fun action(icon: Int, label: String, a: String) =
        Notification.Action.Builder(Icon.createWithResource(this, icon), label, pi(a)).build()

    private fun buildNotification(s: PlayerUiState): Notification {
        val playing = s.state == EngineState.PLAYING
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle(s.title.ifEmpty { "HiFi Player" })
            .setContentText(s.streamInfo)
            .setOngoing(playing)
            .addAction(action(android.R.drawable.ic_media_previous, "Anterior", ACTION_PREV))
            .addAction(action(if (playing) android.R.drawable.ic_media_pause else android.R.drawable.ic_media_play,
                "Play/Pausa", ACTION_TOGGLE))
            .addAction(action(android.R.drawable.ic_media_next, "Siguiente", ACTION_NEXT))
            .setStyle(Notification.MediaStyle().setMediaSession(session.sessionToken).setShowActionsInCompactView(0, 1, 2))
            .build()
    }

    override fun onBind(i: Intent?): IBinder? = null

    override fun onDestroy() {
        scope.cancel()
        if (wakeLock?.isHeld == true) wakeLock?.release()
        session.release()
        super.onDestroy()
    }

    companion object {
        const val CHANNEL = "playback"
        const val NOTIF_ID = 1
        const val ACTION_TOGGLE = "com.hifiplayer.TOGGLE"
        const val ACTION_NEXT = "com.hifiplayer.NEXT"
        const val ACTION_PREV = "com.hifiplayer.PREV"
    }
}
