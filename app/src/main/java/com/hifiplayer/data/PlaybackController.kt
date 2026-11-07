package com.hifiplayer.data

import android.content.Context
import android.util.Log
import com.hifiplayer.audio.AudioFocusManager
import com.hifiplayer.audio.BitPerfectManager
import com.hifiplayer.audio.NativeAudioEngine
import com.hifiplayer.domain.EngineState
import com.hifiplayer.domain.PlayerUiState
import com.hifiplayer.domain.Track
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.util.concurrent.Executors

/**
 * Orquesta cola + motor nativo. Toda la lógica corre en UN hilo (executor) => sin condiciones de carrera.
 * Aquí se enchufará el AutomixPlanner (Fase 5): decide qué pista encolar con queueNext().
 */
class PlaybackController(private val context: Context) {
    private val engine = NativeAudioEngine
    private val bitPerfect = BitPerfectManager(context)
    private val dispatcher = Executors.newSingleThreadExecutor { Thread(it, "hifi-controller") }.asCoroutineDispatcher()
    private val scope = CoroutineScope(SupervisorJob() + dispatcher)

    private val _ui = MutableStateFlow(PlayerUiState())
    val ui: StateFlow<PlayerUiState> = _ui

    private var queue: List<Track> = emptyList()
    private var index = -1
    private var lastTransitions = 0
    private var streamInfo = ""
    private var bpInfo = ""
    private var bpEnabled = true
    private var lastState = EngineState.IDLE
    private var resumeOnFocus = false
    private var ducked = false
    private var modePoll = 0

    private val focus = AudioFocusManager(context, object : AudioFocusManager.Listener {
        override fun onFocusLost(permanent: Boolean) {
            scope.launch {
                val wasPlaying = engine.state() == EngineState.PLAYING.ordinal
                Log.d(TAG, "foco perdido (permanente=$permanent, sonaba=$wasPlaying)")
                if (wasPlaying) engine.pause()
                resumeOnFocus = wasPlaying && !permanent
                if (permanent) restoreVolume()   // si luego se reanuda a mano, que suene a volumen pleno
                publish()
            }
        }
        override fun onDuck() {
            scope.launch {
                Log.d(TAG, "atenuando a ${(DUCK_VOLUME * 100).toInt()}%")
                ducked = true
                engine.setVolume(DUCK_VOLUME)
            }
        }
        override fun onFocusGained() {
            scope.launch {
                Log.d(TAG, "foco recuperado (reanudar=$resumeOnFocus, atenuado=$ducked)")
                restoreVolume()
                if (resumeOnFocus) { resumeOnFocus = false; engine.play(); publish() }
            }
        }
    })

    private fun restoreVolume() {
        if (ducked) { ducked = false; engine.setVolume(1f); Log.d(TAG, "volumen restaurado al 100%") }
    }

    /** Pide foco y arranca/reanuda. Sin foco (p. ej. durante una llamada) no suena. */
    private fun startPlayback(): Boolean {
        if (!focus.request()) { Log.w(TAG, "foco de audio denegado: no se reproduce"); return false }
        resumeOnFocus = false
        restoreVolume()
        return engine.play()
    }

    init {
        scope.launch { while (isActive) { tick(); delay(250) } }
    }

    // ---- API pública
    fun setQueue(tracks: List<Track>) = scope.launch { queue = tracks; playIndex(0) }
    fun play() = scope.launch { Log.d(TAG, "cmd play"); startPlayback(); publish() }
    fun pause() = scope.launch { Log.d(TAG, "cmd pause"); resumeOnFocus = false; engine.pause(); publish() }
    fun togglePlayPause() = scope.launch {
        Log.d(TAG, "cmd toggle (estado=${engine.state()})")
        if (engine.state() == EngineState.PLAYING.ordinal) { resumeOnFocus = false; engine.pause() } else startPlayback()
        publish()
    }
    fun next() = scope.launch { Log.d(TAG, "cmd next"); if (index + 1 < queue.size) playIndex(index + 1) }
    fun previous() = scope.launch {
        Log.d(TAG, "cmd previous")
        if (engine.positionMs() > 3000 || index <= 0) engine.seek(0) else playIndex(index - 1)
        publish()
    }
    fun seek(ms: Long) = scope.launch { Log.d(TAG, "cmd seek $ms"); engine.seek(ms); publish() }
    fun setBitPerfect(enabled: Boolean) = scope.launch {
        bpEnabled = enabled
        engine.setBitPerfect(enabled)
        if (!enabled) { bitPerfect.clear(); bpInfo = "Bit-perfect desactivado" }
        Log.d(TAG, "bitPerfect=$enabled (aplica desde la siguiente pista/reproducción)")
        publish()
    }

    // ---- interno
    private fun fdLoad(track: Track, queueOnly: Boolean): Boolean =
        context.contentResolver.openFileDescriptor(track.uri, "r")?.use { pfd ->
            if (queueOnly) engine.queueNext(pfd.fd) else engine.load(pfd.fd)
        } ?: false

    private fun playIndex(start: Int) {
        var i = start
        while (i in queue.indices) {
            if (fdLoad(queue[i], false)) {
                index = i
                lastTransitions = 0
                if (bpEnabled) engine.format().let { bpInfo = bitPerfect.apply(it[0], it[1], it[2]) }
                startPlayback()
                queueNext()
                streamInfo = engine.describe()
                Log.d(TAG, "Reproduciendo [$i] ${queue[i].title} | $streamInfo | $bpInfo")
                publish(); return
            }
            Log.e(TAG, "No se pudo abrir '${queue[i].title}', se omite")
            i++
        }
        engine.stop(); publish()
    }

    private fun queueNext() {
        queue.getOrNull(index + 1)?.let { if (!fdLoad(it, true)) Log.w(TAG, "queueNext falló: ${it.title}") }
    }

    private fun tick() {
        if (queue.isEmpty()) return
        val st = EngineState.from(engine.state())
        // Atenuado y de pronto empieza una llamada (se contestó mientras sonaba): pausar hasta que termine
        if (ducked && st == EngineState.PLAYING) {
            modePoll = (modePoll + 1) % 4            // tick de 250 ms -> comprobar 1 vez por segundo
            if (modePoll == 0 && focus.inConversation()) {
                Log.d(TAG, "conversacion en curso mientras estaba atenuado -> pausa hasta que termine")
                engine.pause(); resumeOnFocus = true
            }
        } else modePoll = 0
        val tr = engine.transitions()
        if (tr > lastTransitions) {
            index += tr - lastTransitions
            lastTransitions = tr
            Log.d(TAG, "Gapless -> pista [$index]")
            queueNext()
            streamInfo = engine.describe()
        }
        if (st == EngineState.ENDED && index + 1 < queue.size) { playIndex(index + 1); return }
        if (st == EngineState.ENDED) focus.abandon()   // fin de la cola: liberar el foco
        if (st != lastState) { Log.d(TAG, "Estado motor: $lastState -> $st"); lastState = st }
        publish()
    }

    private fun publish() {
        _ui.value = PlayerUiState(
            title = queue.getOrNull(index)?.title.orEmpty(),
            state = EngineState.from(engine.state()),
            positionMs = engine.positionMs(),
            durationMs = engine.durationMs(),
            index = index, queueSize = queue.size,
            streamInfo = streamInfo, bitPerfectInfo = bpInfo, bitPerfectEnabled = bpEnabled,
        )
    }

    private companion object {
        const val TAG = "HiFi.Controller"
        /** Ganancia lineal durante la atenuación: 0.4 = 40 % de amplitud (unos -8 dB). */
        const val DUCK_VOLUME = 0.4f
    }
}
