package com.hifiplayer.audio

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.util.Log

/**
 * Foco de audio con política de atenuación ("ducking"):
 *  - Interrupción corta (aviso, nota de voz, ringtone...)  -> ATENUAR y seguir sonando.
 *  - Llamada / videollamada EN CURSO                       -> PAUSAR (no se superpone música a una conversación).
 *  - Otra app se queda el foco (otro reproductor, vídeo)   -> PAUSAR sin reanudar sola.
 * Los callbacks llegan en el hilo principal; el controlador los reenvía a su propio hilo.
 */
class AudioFocusManager(context: Context, private val listener: Listener) {
    interface Listener {
        /** Pausar. [permanent] = otra app se quedó con el foco: no reanudar sola. */
        fun onFocusLost(permanent: Boolean)
        /** Bajar el volumen y seguir reproduciendo. */
        fun onDuck()
        /** Foco recuperado: restaurar el volumen y reanudar si se había pausado por pérdida transitoria. */
        fun onFocusGained()
    }

    private val am = context.getSystemService(AudioManager::class.java)
    private var held = false

    private val focusRequest: AudioFocusRequest = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
        .setAudioAttributes(
            AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build())
        .setOnAudioFocusChangeListener { change -> onChange(change) }
        .build()

    fun request(): Boolean {
        if (held) return true
        val r = am.requestAudioFocus(focusRequest)
        held = r == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        Log.d(TAG, "requestAudioFocus -> $r (concedido=$held)")
        return held
    }

    fun abandon() {
        if (!held) return
        am.abandonAudioFocusRequest(focusRequest)
        held = false
        Log.d(TAG, "abandonAudioFocus")
    }

    /** true si hay una llamada o videollamada en curso (sonando NO cuenta: ahí se atenúa). */
    fun inConversation(): Boolean = isConversation(am.mode)   // una sola lectura (cada una es una llamada binder)

    private fun isConversation(mode: Int) =
        mode == AudioManager.MODE_IN_CALL || mode == AudioManager.MODE_IN_COMMUNICATION

    private fun duckOrPause(mode: Int) {
        if (isConversation(mode)) listener.onFocusLost(false) else listener.onDuck()
    }

    private fun onChange(change: Int) {
        val mode = am.mode
        Log.d(TAG, "onAudioFocusChange($change) modoAudio=$mode")
        when (change) {
            AudioManager.AUDIOFOCUS_GAIN -> listener.onFocusGained()
            AudioManager.AUDIOFOCUS_LOSS -> {
                held = false
                am.abandonAudioFocusRequest(focusRequest)
                listener.onFocusLost(true)
            }
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> duckOrPause(mode)
            // Notas de voz y avisos suelen pedir foco "transitorio" a secas: se atenúa igual (ver DUCK_ON_TRANSIENT).
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT ->
                if (DUCK_ON_TRANSIENT) duckOrPause(mode) else listener.onFocusLost(false)
        }
    }

    companion object {
        private const val TAG = "HiFi.Focus"
        /** true: también se atenúa ante pérdidas transitorias "normales" (notas de voz). false: se pausa. */
        const val DUCK_ON_TRANSIENT = true
    }
}
