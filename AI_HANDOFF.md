# AI_HANDOFF.md — Contexto del proyecto para asistentes de IA
<!-- Mantener actualizado al cierre de cada sesión. Última actualización: 2026-11-10 (v5: integración real hot-plug en estructura existente) -->

## 0. Cómo usar este archivo (leer primero)
1. Lee este archivo completo y `PENDIENTES.md` (protocolo de pruebas en hardware).
2. Lee los archivos clave listados en §3.
3. Pregunta al usuario qué ítem del roadmap (§7) o de los pendientes (§5/§6) se aborda hoy.
4. Trabaja de forma ITERATIVA y por módulos (regla del prompt fundacional: NUNCA generar toda la app en una sola respuesta).
5. Al terminar: actualiza §4 (historial) y §5/§6/§7 de este archivo y commitea con mensaje descriptivo.
6. NOTA OPERATIVA: raw.githubusercontent cachea contenido; verificar cambios con `?nocache=` o la API de commits antes de afirmar que "no hay cambios" (falsa alarma ocurrida el 2026-11-10).

## 1. Visión original (resumen del prompt fundacional — ver Apéndice A)
App Android de reproducción Hi-Fi para audiófilos + DJ/Automix inteligente.
Roles exigidos al asistente: Dev Senior Android + Ingeniero de Audio (DSP) + Especialista C++/NDK.
Alcance comprometido: motor FLAC/WAV/ALAC/DSD lossless; salida bit-perfect (bypass mixer Android);
gapless; salidas altavoz/Bluetooth (LDAC/aptX HD)/AUX/USB-C DAC; biblioteca con escaneo profundo y
carpetas/géneros; MIR (BPM/key/waveform) en segundo plano persistente; DSP (EQ 13 bandas,
ReplayGain/LUFS con pre-amp, compresor Threshold/Ratio/Attack/Release); Automix Modo Carpeta y
Modo Biblioteca con comportamiento DJ real (beatmatching, frases, EQ en crossfade, efectos echo
out/HP-LP/frenado de vinilo); resistencia a Doze; logs exhaustivos en capas DSP y Automix.
Método: fases iterativas por módulos.

## 2. Decisiones técnicas ya adoptadas (con rationale)
- DT-01 Oboe/AAudio + motor C++ propio (vs ExoPlayer/Superpowered/FMOD): control total del path
  bit-perfect, sharing mode y buffers.
- DT-02 Decodificación dr_libs (dr_flac/dr_wav) en C++; I/O por file descriptor (dup(fd)) => SAF sin permisos especiales.
- DT-03 Resampler propio Kaiser (~100 dB, polifásico, streaming-safe).
- DT-04 SampleConvert.h header-only testeable en PC; applyGain target=1.0 no toca muestras.
- DT-05 Dithering TPDF en int32->float.
- DT-06 Negociación en cascada con verificación del modo REAL (Exclusive/MMAP) + cache mNegCache;
  mezclador BIT_PERFECT API 34+ gestionado desde Kotlin (OutputRouter).
- DT-07 Códigos OutFmt compartidos: 0=PCM_16, 1=PCM_24 packed, 2=PCM_32, 3=FLOAT.
- DT-08 Tags de log: HiFi, HiFi.Route, HiFi.Engine, HiFi.Controller, HiFi.Service; futuros HiFi.DSP, HiFi.MIX, HiFi.MIR.
- DT-09 Single-thread executor (`Executors.newSingleThreadExecutor`) en PlaybackController: toda la lógica
  Kotlin corre en UN hilo => sin condiciones de carrera, sin locks en la capa de control.
- DT-10 Estado de UI unificado (`PlayerUiState`) expuesto como `StateFlow` único; ViewModel es passthrough al controller.

## 3. Arquitectura actual y CONTRATOS QUE NO SE ROMPEN
Flujo: PcmSource(fd) -> hilo decoder (+Resampler) -> RingBuffer SPSC -> callback Oboe -> DAC.
- C-01: onAudioReady sin locks, malloc ni logs.
- C-02: bit-perfect = modo PURE sin DSP; features nuevas respetan PURE o se declaran PROCESSED (DD-01).
- C-03: timeline gapless por mBoundaries (frames de SALIDA); positionMs() extrapola con mFramesRendered.
- C-04: hot-plug: AudioDeviceCallback (@Volatile routeKey) refresca plan; onErrorAfterClose gestiona.
- C-05: recuperación hot-plug NATIVA (estado Suspended=6): play()=recuperar ya; pause()=cancelar
  recovery (queda Paused con posición); seekMs() válido en Suspended; timeout 20 s -> Paused. Kotlin
  solo observa el estado vía polling (250ms tick) y deriva avisos desde telemetría.
- C-06: PlaybackController es la única autoridad de estado; ViewModel y Service solo consumen su StateFlow.
- C-07: `applyPlan()` se ejecuta SIEMPRE antes de `startPlayback()` y tras `onRouteChanged()`.
- C-08: `tick()` de 250ms es el reloj del sistema: detecta gapless transitions, estados, focus duck, recovery.
  Archivos clave:
- cpp: AudioEngine.cpp/.h, OutputPlanner.h, Resampler.h, SampleConvert.h, RingBuffer.h,
  PcmSource.cpp/.h, jni_bridge.cpp, Log.h, CMakeLists.txt
- kotlin: audio/{OutputRouter,NativeAudioEngine,BitPerfectManager,AudioFocusManager,OutputTelemetry}.kt,
  data/PlaybackController.kt, service/PlaybackService.kt, HiFiApp.kt,
  ui/{MainActivity,PlayerViewModel}.kt, domain/Models.kt

## 4. Historial de sesiones
- 2026-11-09 | Fase 1 revisada (motor gapless FLAC/WAV, Service+MediaSession, Compose MVVM). Repo público.
- 2026-11-09 | Bugfix: OutputRouter.kt:78 `IntArray.mapNotNull` -> `.asSequence().mapNotNull(::mapEncoding)...`.
- 2026-11-10 | Fase 2 mejorada y pusheada (commits 483b558..2f5695c): dither TPDF, verificación buffer
  size + WARNING telemetría, @Volatile routeKey, mResamplerActive.
- 2026-11-10 | Prompt fundacional incorporado al contexto; AI_HANDOFF v2 (gap analysis + roadmap maestro).
- 2026-11-10 | Fase 7-parcial C++: auto-recuperación hot-plug (Suspended=6, frame de reanudación
  compensado por resampler, hilo recovery try_lock 800ms/timeout 20s, telemetría suspend/recover).
- 2026-11-10 | Integración Kotlin del hot-plug sobre la estructura REAL del proyecto (no capa genérica):
  - Models.kt: `EngineState.SUSPENDED` añadido; `PlayerUiState` con `bitPerfectWarning`+`recoveryNotice`.
  - PlaybackController: tick() reconoce Suspended, deriva warnings desde `outputInfo()`, onRouteChanged
    llama a `applyPlan()` antes de `reopenCurrent()` para que el recovery use el plan nuevo.
  - PlaybackService: `SUSPENDED -> PlaybackState.STATE_BUFFERING`; notificación muestra "Esperando DAC…"
    con `setOngoing(true)`; wakelock ya existente cubre el estado.
  - MainActivity: 3 banners condicionales en PlayerScreen (Suspended, bitPerfectWarning, recoveryNotice).
    Pendiente: validación en hardware (pruebas 1–7).

## 5. Refinamientos pendientes de code review (NO bloqueantes)
- [ ] Dither TPDF en fromFloat() para targets enteros en rutas ya no bit-perfect.
- [ ] RNG más barato (xorshift64*) si underruns en gama baja a 384 kHz.
- [ ] mResamplerActive: exponer getter para UI ("SRC activo") o eliminar.
- [ ] mixerDevice (OutputRouter): confinar a un hilo o proteger con mutex.
- [ ] Higiene repo: .gitignore (.gradle/, .idea/, app/build/, build/, app/.cxx/, local.properties),
  `git rm -r --cached ...`, commits descriptivos, unificar branches (master/main/MasterDaCv1).
- [ ] Consolidar el recovery Kotlin (3 reintentos cada 2s en tick() para ERROR) con el recovery C++
  (800ms/timeout 20s en recoveryLoop): hoy conviven pero el C++ es más rápido y preciso.
- [ ] Validar hot-plug en hardware: pruebas 1–7 de §9.

## 6. Deuda vs especificación original (gap analysis)
HECHO:
- Motor FLAC/WAV gapless; bit-perfect USB DAC (exclusive/MMAP/mixer BP API34); resampler HQ;
  dither TPDF; telemetría con warnings; hot-plug: detección + auto-recuperación con reanudación
  en el mismo frame (C++ y Kotlin).
- ForegroundService + MediaSession + wakelock + becoming-noisy; UI Compose básica; AudioFocusManager
  propio con ducking y detección de conversación; Doze parcial (permiso request).
  PENDIENTE:
- ALAC y DSD/DoP; reporte codec Bluetooth (LDAC/aptX HD); escaneo biblioteca + Room;
  MIR (BPM/key/waveform); DSP (EQ13/ReplayGain-LUFS/compresor); Automix/DJ; UI biblioteca/DJ;
  Doze completo (WorkManager, ForegroundServiceType correcto en API 34+).

## 7. Roadmap maestro (alineado al prompt fundacional)
FASE 3 — Biblioteca y persistencia: escaneo SAF tree + MediaStore; Room (tracks, playlists, analysis);
carpetas/géneros; "reproducir carpeta"; cola persistida.
FASE 4 — MIR offline: WorkManager + kissfft/pffft; BPM (spectral flux + comb), key (chroma +
Krumhansl-Schmuckler), waveform (picos min/max), ReplayGain/LUFS (libebur128); Room; incremental.
FASE 5 — DSP (modo PROCESSED, DD-01): cadena en hilo decoder: EQ 13 bandas (biquad RBJ), compresor con
lookahead, ganancia ReplayGain+pre-amp en applyGain; bypass total = PURE.
FASE 6 — Automix/DJ: doble deck -> Mixer stage -> RingBuffer único; crossfader; beatmatching (resampler
de ratio dinámico); alineación de frases; EQ dinámica en crossfade; efectos echo out/HP-LP/vinilo;
Modo Carpeta y Modo Biblioteca.
FASE 7 — Formatos y salidas: ALAC; DSD nativo/DoP; reporte codec Bluetooth.
(Hot-plug auto-recovery: HECHO C+++Kotlin el 2026-11-10, pendiente validación hardware).
FASE 8 — UI final y endurecimiento: biblioteca Compose, vista DJ, ajustes, Doze completo,
pruebas de instrumento + protocolo hardware (PENDIENTES.md).
Criterio de cierre de fase: compila, pruebas logcat OK, actualización de §4/§6.

## 8. Decisiones de diseño emergentes
- DD-01 DOS caminos: PURE (bit-perfect sin DSP) y PROCESSED (DSP/Automix); selección explícita y visible.
- DD-02 Todo DSP en hilo decoder, nunca en callback Oboe (C-01).
- DD-03 Automix: dos cadenas decodificadas sumadas en Mixer stage -> ÚNICO RingBuffer (no duplicar Oboe).
- DD-04 MIR offline y persistente (Room); automix consume metadatos ya analizados.
- DD-05 Permisos: preferir SAF+FD; MANAGE_EXTERNAL_STORAGE solo si es imprescindible (política Play).
- DD-06 Recovery hot-plug (nativo): hilo con mMutex.try_lock() (anti-deadlock con join bajo mMutex);
  frame de reanudación = mStartOffset + rendered*(src/out) (compensa resampler); ring descartado
  (se reanuda donde el usuario OYÓ); auto-recovery solo si reproducía; en pausa, play() recupera.
- DD-07 Control single-thread: PlaybackController usa executor de un hilo (DT-09). Las llamadas a la
  fachada JNI (NativeAudioEngine) son thread-safe en C++, pero toda la lógica de estado (cola, index,
  focus, plan) vive en un solo hilo Kotlin. Esto permite evitar locks y simplifica el reasoning.
- DD-08 UI passthrough: PlayerViewModel es un wrapper de una línea sobre PlaybackController.ui. No
  duplica estado. Cualquier feature de UI consume el StateFlow del controller, no un estado propio.
- DD-09 Plan refresh en hot-plug: onRouteChanged llama a applyPlan() ANTES de reopenCurrent() para que
  el motor use las capacidades del DAC nuevo (o ausencia de DAC) en la renegociación.
- DD-10 Telemetría como fuente de verdad de avisos: bitPerfectWarning y recoveryNotice se derivan en
  publish() desde `engine.outputInfo()` (cadena "clave=valor;..."). Nunca se guardan por separado.

## 9. Pruebas y observabilidad
- Logcat: HiFi / HiFi.Route / HiFi.Engine / HiFi.Controller / HiFi.Service (+ futuros HiFi.DSP, HiFi.MIX, HiFi.MIR).
- Líneas clave: "NEG ...", "Buffer configurado: ...", "Stream final: mode=...;buffer=...;neg=...",
  "WARNING: exclusive mode unavailable...", "SUSPEND: DAC desconectado; reanudacion preparada en frame N",
  "RECOVER OK: stream reabierto y reanudado tras K intento(s)", "RECOVER timeout ...",
  "cambio de dispositivo USB: plan refrescado y renegociando la salida" (HiFi.Controller).
- Telemetría (outputInfo): mode/excl/mmap/api/dev/rate/fmt/src/bits/resampled/attempts/why/buffer/neg
  + append: suspend=1;resumeFrame=N; recover=ok|timeout|cancel;attempts=K.
- UI: banner ámbar "⏳ Esperando DAC…" si state==SUSPENDED; banner rojo si bitPerfectWarning;
  banner rojo si recoveryNotice. Service notifica "Esperando DAC… reanudación automática" en Suspended.
- Hardware: DAC KT02H20 / JM12; PCM 32-bit/384 kHz; pares gapless 10+11 y 20+21.
- Hot-plug (pruebas 1–7 en PENDIENTES.md):
  1) Reconexión <20 s reanuda en mismo frame y modo EXCL/MIXER_BP (logcat: "RECOVER OK").
  2) Desconexión >20 s -> recover=timeout y Paused, play reanuda (logcat: "RECOVER timeout").
  3) Pausa durante suspensión cancela recovery (recover=cancel).
  4) Seek en suspensión redefine punto de reanudación.
  5) Desconexión en pausa no lanza recovery (el motor nativo no lo hace cuando no reproducía).
  6) Gapless con cola encolada sobrevive reconexión (mNext no se invalida).
  7) Error no-disconnect mantiene comportamiento Error clásico (sin auto-recovery).
- Casos generales: hot-plug en reproducción, Doze 10-15 min, llamada entrante, otro reproductor,
  cambio de carpeta con formatos mixtos.

## Apéndice A — Prompt fundacional (verbatim, abreviado en formato)
"Actúa como Desarrollador Senior de Android, Experto en Ingeniería de Audio (DSP) y Especialista en
C++/Android NDK. Objetivo: diseñar arquitectura, stack y código base de una app Android de
reproducción de música de Alta Fidelidad (Hi-Fi) para audiófilos, con funciones avanzadas de
DJ/Automix inteligente.
Habilidades requeridas: audio nativo C++/NDK bit-perfect y lossless de baja latencia (Oboe,
Superpowered o FMOD); DSP (EQ paramétrica/gráfica 13 bandas, compresión dinámica, normalización
ReplayGain/LUFS, filtros de transición); MIR (BPM, transitorios, key en segundo plano); Android
avanzado (Foreground Services, MediaSession, corrutinas, Jetpack Compose, permisos
MANAGE_EXTERNAL_STORAGE/SAF); lógica DJ (beatmatching, alineación de frases, EQ dinámica en
crossfade, efectos echo out / filtros HP-LP / frenado de vinilo).
Especificaciones: formatos FLAC, WAV, ALAC, DSD y lossless; salida bit-perfect compatible con
altavoz, Bluetooth (LDAC/aptX HD), AUX 3.5mm y USB-C DAC; gapless; escaneo local profundo con
organización por carpetas/géneros y análisis en segundo plano de BPM/tonalidad/forma de onda
persistente; módulo DSP (EQ 13 bandas, ReplayGain/LUFS con pre-amp, compresor ajustable
Threshold/Ratio/Attack/Release); Automix Modo Carpeta (coherencia de género y BPM) y Modo
Biblioteca (transiciones adaptativas según diferencia de ritmo) con comportamiento DJ real;
manejo de Doze Mode; logs exhaustivos en capas DSP y Automix.
Entregables Fase 1: arquitectura y stack (Oboe vs ExoPlayer vs NDK), build.gradle.kts con NDK/C++,
esqueleto del motor reproduciendo FLAC/WAV sin pérdidas, y hoja de ruta de fases siguientes.
Método: NO generar toda la app en una sola respuesta; trabajo iterativo por módulos."