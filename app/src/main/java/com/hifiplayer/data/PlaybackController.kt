package com.hifiplayer.data

import android.content.Context
import android.os.SystemClock
import android.util.Log
import com.hifiplayer.audio.AudioFocusManager
import com.hifiplayer.audio.NativeAudioEngine
import com.hifiplayer.audio.OutputRouter
import com.hifiplayer.audio.OutputTelemetry
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
    private val dispatcher = Executors.newSingleThreadExecutor { Thread(it, "hifi-controller") }.asCoroutineDispatcher()
    private val scope = CoroutineScope(SupervisorJob() + dispatcher)
    private val router = OutputRouter(context) { scope.launch { onRouteChanged() } }

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
    private var lastPlan: OutputRouter.Plan? = null
    private var lastPosMs = 0L
    private var lastPlaying = false
    private var recoverTries = 0
    private var lastRecoverAt = 0L

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
        if (!enabled) router.clearMixer()
        Log.d(TAG, "bitPerfect=$enabled -> reabriendo la salida")
        reopenCurrent(resume = engine.state() == EngineState.PLAYING.ordinal, posMs = engine.positionMs())
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
                applyPlan()
                startPlayback()
                queueNext()
                refreshTelemetry()
                Log.d(TAG, "Reproduciendo [$i] ${queue[i].title} | $streamInfo | $bpInfo")
                publish(); return
            }
            Log.e(TAG, "No se pudo abrir '${queue[i].title}', se omite")
            i++
        }
        engine.stop(); publish()
    }

    /** Lee las capacidades del DAC y se las pasa al motor para que negocie frecuencia/formato (ver OutputRouter). */
    private fun applyPlan() {
        val f = engine.format()                       // [frecuencia, canales, bits] de la pista cargada
        val p = router.plan(f[0], f[1], f[2], bpEnabled)
        lastPlan = p
        engine.setOutputPlan(p.deviceId, p.rates, p.formats, p.forcedRate, p.forcedFmt)
    }

    private fun refreshTelemetry() {
        streamInfo = engine.describe()
        bpInfo = OutputTelemetry.line(engine.outputInfo(), lastPlan?.deviceName, lastPlan?.mixerNote.orEmpty())
    }

    /** Recarga la pista actual y renegocia la salida, conservando la posición (cambio de DAC, error, conmutador). */
    private fun reopenCurrent(resume: Boolean, posMs: Long) {
        if (index !in queue.indices) return
        if (!fdLoad(queue[index], false)) { Log.e(TAG, "no se pudo reabrir '${queue[index].title}'"); return }
        lastTransitions = 0
        applyPlan()
        if (posMs > 0) engine.seek(posMs)
        if (resume) startPlayback()
        queueNext()
        refreshTelemetry()
        Log.d(TAG, "salida reabierta en $posMs ms | $streamInfo | $bpInfo")
        publish()
    }

    private fun onRouteChanged() {
        if (index !in queue.indices) return
        val st = EngineState.from(engine.state())
        if (st == EngineState.IDLE) return

        // ═══ HOT-PLUG: refrescar plan YA (el recovery nativo lo usa en <800ms) ═══
        applyPlan()

        Log.d(TAG, "cambio de dispositivo USB: plan refrescado y renegociando la salida")
        val pos = if (st == EngineState.ERROR || st == EngineState.SUSPENDED) lastPosMs else engine.positionMs()
        reopenCurrent(resume = if (st == EngineState.ERROR || st == EngineState.SUSPENDED) lastPlaying else st == EngineState.PLAYING, posMs = pos)
    }

    private fun queueNext() {
        queue.getOrNull(index + 1)?.let { if (!fdLoad(it, true)) Log.w(TAG, "queueNext falló: ${it.title}") }
    }

    private fun tick() {
        if (queue.isEmpty()) return
        val st = EngineState.from(engine.state())

        // ═══ HOT-PLUG: reconocer estado Suspended (6) del motor ═══
        if (st != EngineState.ERROR && st != EngineState.SUSPENDED) {
            lastPosMs = engine.positionMs()
            lastPlaying = st == EngineState.PLAYING
            recoverTries = 0
        } else if (st == EngineState.ERROR) {
            // p. ej. DAC desconectado (motor antiguo sin Suspended): reintentar (máx. 3 veces) en la misma posición
            val now = SystemClock.elapsedRealtime()
            if (recoverTries < 3 && now - lastRecoverAt > 2000) {
                recoverTries++; lastRecoverAt = now
                Log.w(TAG, "motor en ERROR: reintento $recoverTries/3 en $lastPosMs ms")
                reopenCurrent(resume = lastPlaying, posMs = lastPosMs)
                return
            }
        }
        // Estado Suspended: el motor nativo maneja la recuperación internamente (C++ recovery loop).
        // Kotlin solo muestra el estado y espera que el motor recupere o timeout.

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
        // ═══ HOT-PLUG: derivar avisos desde telemetría ═══
        val telemetry = engine.outputInfo()
        val bpWarning = telemetry.contains("WARNING: exclusive mode unavailable")
        val recoveryNotice = when {
            telemetry.contains("recover=timeout") -> "DAC no reconectado a tiempo: en pausa (posición conservada)"
            else -> null
        }

        _ui.value = PlayerUiState(
            title = queue.getOrNull(index)?.title.orEmpty(),
            state = EngineState.from(engine.state()),
            positionMs = engine.positionMs(),
            durationMs = engine.durationMs(),
            index = index, queueSize = queue.size,
            streamInfo = streamInfo, bitPerfectInfo = bpInfo, bitPerfectEnabled = bpEnabled,
            bitPerfectWarning = bpWarning,
            recoveryNotice = recoveryNotice,
        )
    }

    private companion object {
        const val TAG = "HiFi.Controller"
        /** Ganancia lineal durante la atenuación: 0.4 = 40 % de amplitud (unos -8 dB). */
        const val DUCK_VOLUME = 0.4f
    }
}