package com.hifiplayer.service

import android.app.*
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.os.IBinder
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import com.hifiplayer.HiFiApp
import com.hifiplayer.domain.EngineState
import com.hifiplayer.domain.PlayerUiState
import com.hifiplayer.ui.MainActivity
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.collect

class PlaybackService : Service() {
    private lateinit var session: MediaSession
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var wakeLock: PowerManager.WakeLock? = null
    private val ctrl get() = (application as HiFiApp).controller

    private var pubState: EngineState? = null
    private var pubIndex = -1
    private var pubDuration = -1L
    private var pubPos = 0L
    private var pubAt = 0L
    private var lastNotifKey = ""

    private val noisyReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            Log.d(TAG, "ACTION_AUDIO_BECOMING_NOISY -> pausa")
            ctrl.pause()
        }
    }

    override fun onCreate() {
        super.onCreate()
        Log.d(TAG, "onCreate")
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL, "Reproducción", NotificationManager.IMPORTANCE_LOW).apply {
                lockscreenVisibility = Notification.VISIBILITY_PUBLIC
                setShowBadge(false)
            })

        session = MediaSession(this, "HiFiSession").apply {
            setSessionActivity(openAppIntent())
            setCallback(object : MediaSession.Callback() {
                override fun onPlay() { Log.d(TAG, "session.onPlay"); ctrl.play() }
                override fun onPause() { Log.d(TAG, "session.onPause"); ctrl.pause() }
                override fun onSkipToNext() { Log.d(TAG, "session.onSkipToNext"); ctrl.next() }
                override fun onSkipToPrevious() { Log.d(TAG, "session.onSkipToPrevious"); ctrl.previous() }
                override fun onSeekTo(pos: Long) { Log.d(TAG, "session.onSeekTo($pos)"); ctrl.seek(pos) }
            })
            isActive = true
        }
        ServiceCompat.startForeground(this, NOTIF_ID, buildNotification(ctrl.ui.value),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)

        ContextCompat.registerReceiver(this, noisyReceiver,
            IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY), ContextCompat.RECEIVER_NOT_EXPORTED)

        wakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "HiFi:playback").apply { setReferenceCounted(false) }

        scope.launch {
            ctrl.ui.collect { s ->
                updateWakeLock(s)
                publishToSession(s)
                val key = "${s.title}|${s.state}"
                if (key != lastNotifKey) {
                    lastNotifKey = key
                    nm.notify(NOTIF_ID, buildNotification(s))
                }
            }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val action = intent?.action
        Log.d(TAG, "onStartCommand action=$action")
        when (action) {
            ACTION_TOGGLE -> ctrl.togglePlayPause()
            ACTION_NEXT -> ctrl.next()
            ACTION_PREV -> ctrl.previous()
        }
        return START_NOT_STICKY
    }

    private fun updateWakeLock(s: PlayerUiState) {
        val wl = wakeLock ?: return
        if (s.state == EngineState.PLAYING) {
            if (!wl.isHeld) { wl.acquire(30 * 60 * 1000L); Log.d(TAG, "wakelock ON") }
        } else if (wl.isHeld) { wl.release(); Log.d(TAG, "wakelock OFF") }
    }

    private fun publishToSession(s: PlayerUiState) {
        val now = SystemClock.elapsedRealtime()
        val predicted = if (pubState == EngineState.PLAYING) pubPos + (now - pubAt) else pubPos
        val jumped = kotlin.math.abs(s.positionMs - predicted) > 1500
        val changed = s.state != pubState || s.index != pubIndex || s.durationMs != pubDuration
        if (!changed && !jumped) return

        if (pubState == null || s.index != pubIndex || s.durationMs != pubDuration) {
            session.setMetadata(MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, s.title.ifEmpty { "HiFi Player" })
                .putString(MediaMetadata.METADATA_KEY_ARTIST, "Pista ${s.index + 1} de ${s.queueSize}")
                .putString(MediaMetadata.METADATA_KEY_ALBUM, s.streamInfo)
                .putLong(MediaMetadata.METADATA_KEY_DURATION, s.durationMs)
                .build())
        }
        val playing = s.state == EngineState.PLAYING
        val st = when (s.state) {
            EngineState.PLAYING -> PlaybackState.STATE_PLAYING
            EngineState.PAUSED, EngineState.READY -> PlaybackState.STATE_PAUSED
            EngineState.SUSPENDED -> PlaybackState.STATE_BUFFERING   // ═══ HOT-PLUG ═══
            EngineState.ERROR -> PlaybackState.STATE_ERROR
            else -> PlaybackState.STATE_STOPPED
        }
        session.setPlaybackState(PlaybackState.Builder()
            .setActions(ACTIONS)
            .setState(st, s.positionMs, if (playing) 1f else 0f)
            .build())
        pubState = s.state; pubIndex = s.index; pubDuration = s.durationMs; pubPos = s.positionMs; pubAt = now
        Log.d(TAG, "sesion publicada: $st pos=${s.positionMs} dur=${s.durationMs} '${s.title}'")
    }

    private fun openAppIntent() = PendingIntent.getActivity(
        this, 0,
        Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

    private fun pi(action: String) = PendingIntent.getService(
        this, action.hashCode(),
        Intent(this, PlaybackService::class.java).setAction(action),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

    private fun action(icon: Int, label: String, a: String) =
        Notification.Action.Builder(Icon.createWithResource(this, icon), label, pi(a)).build()

    private fun buildNotification(s: PlayerUiState): Notification {
        val playing = s.state == EngineState.PLAYING
        val suspended = s.state == EngineState.SUSPENDED   // ═══ HOT-PLUG ═══
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle(s.title.ifEmpty { "HiFi Player" })
            .setContentText(when {
                suspended -> "Esperando DAC… reanudación automática"
                else -> s.streamInfo
            })
            .setContentIntent(openAppIntent())
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setCategory(Notification.CATEGORY_TRANSPORT)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setOngoing(playing || suspended)   // ═══ HOT-PLUG: ongoing también en Suspended ═══
            .addAction(action(android.R.drawable.ic_media_previous, "Anterior", ACTION_PREV))
            .addAction(action(if (playing) android.R.drawable.ic_media_pause else android.R.drawable.ic_media_play,
                "Play/Pausa", ACTION_TOGGLE))
            .addAction(action(android.R.drawable.ic_media_next, "Siguiente", ACTION_NEXT))
            .setStyle(Notification.MediaStyle().setMediaSession(session.sessionToken).setShowActionsInCompactView(0, 1, 2))
            .build()
    }

    override fun onBind(i: Intent?): IBinder? = null

    override fun onDestroy() {
        Log.d(TAG, "onDestroy")
        runCatching { unregisterReceiver(noisyReceiver) }
        scope.cancel()
        if (wakeLock?.isHeld == true) wakeLock?.release()
        session.release()
        super.onDestroy()
    }

    companion object {
        private const val TAG = "HiFi.Service"
        const val CHANNEL = "playback"
        const val NOTIF_ID = 1
        const val ACTION_TOGGLE = "com.hifiplayer.TOGGLE"
        const val ACTION_NEXT = "com.hifiplayer.NEXT"
        const val ACTION_PREV = "com.hifiplayer.PREV"
        private const val ACTIONS = PlaybackState.ACTION_PLAY or PlaybackState.ACTION_PAUSE or
                PlaybackState.ACTION_PLAY_PAUSE or PlaybackState.ACTION_SKIP_TO_NEXT or
                PlaybackState.ACTION_SKIP_TO_PREVIOUS or PlaybackState.ACTION_SEEK_TO
    }
}