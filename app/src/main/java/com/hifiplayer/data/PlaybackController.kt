package com.hifiplayer.data

import android.content.Context
import android.util.Log
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

    init {
        scope.launch { while (isActive) { tick(); delay(250) } }
    }

    // ---- API pública
    fun setQueue(tracks: List<Track>) = scope.launch { queue = tracks; playIndex(0) }
    fun play() = scope.launch { engine.play(); publish() }
    fun pause() = scope.launch { engine.pause(); publish() }
    fun togglePlayPause() = scope.launch {
        if (engine.state() == EngineState.PLAYING.ordinal) engine.pause() else engine.play()
        publish()
    }
    fun next() = scope.launch { if (index + 1 < queue.size) playIndex(index + 1) }
    fun previous() = scope.launch {
        if (engine.positionMs() > 3000 || index <= 0) engine.seek(0) else playIndex(index - 1)
        publish()
    }
    fun seek(ms: Long) = scope.launch { engine.seek(ms); publish() }
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
                engine.play()
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
        val tr = engine.transitions()
        if (tr > lastTransitions) {
            index += tr - lastTransitions
            lastTransitions = tr
            Log.d(TAG, "Gapless -> pista [$index]")
            queueNext()
            streamInfo = engine.describe()
        }
        if (st == EngineState.ENDED && index + 1 < queue.size) { playIndex(index + 1); return }
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

    private companion object { const val TAG = "HiFi.Controller" }
}
