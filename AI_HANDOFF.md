# AI_HANDOFF.md — Contexto del proyecto para asistentes de IA
<!-- Mantener actualizado al cierre de cada sesión. Última actualización: 2026-11-10 (v4: capa Kotlin de referencia con hot-plug integrado) -->

## 0. Cómo usar este archivo (leer primero)
1. Lee este archivo completo y `PENDIENTES.md` (protocolo de pruebas en hardware).
2. Lee los archivos clave listados en §3.
3. Pregunta al usuario qué ítem del roadmap (§7) o de los pendientes (§5/§6) se aborda hoy.
4. Trabaja de forma ITERATIVA y por módulos (regla del prompt fundacional: NUNCA generar toda la app en una sola respuesta).
5. Al terminar: actualiza §4 (historial) y §5/§6/§7 de este archivo y commitea con mensaje descriptivo.
6. NOTA OPERATIVA: raw.githubusercontent cachea contenido; verificar cambios con `?nocache=` o la
   API de commits antes de afirmar que "no hay cambios" (falsa alarma ocurrida el 2026-11-10).

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
- DT-08 Tags de log: HiFi, HiFi.Route, HiFi.Engine, HiFi.Ctrl; futuros HiFi.DSP, HiFi.MIX, HiFi.MIR.

## 3. Arquitectura actual y CONTRATOS QUE NO SE ROMPEN
Flujo: PcmSource(fd) -> hilo decoder (+Resampler) -> RingBuffer SPSC -> callback Oboe -> DAC.
- C-01: onAudioReady sin locks, malloc ni logs.
- C-02: bit-perfect = modo PURE sin DSP; features nuevas respetan PURE o se declaran PROCESSED (DD-01).
- C-03: timeline gapless por mBoundaries (frames de SALIDA); positionMs() extrapola con mFramesRendered.
- C-04: hot-plug: AudioDeviceCallback (@Volatile routeKey) refresca plan; onErrorAfterClose gestiona.
- C-05: recuperación hot-plug NATIVA sin JNI nuevo: estado Suspended=6; play()=recuperar ya;
  pause()=cancelar recovery (queda Paused con posición); seekMs() válido en Suspended;
  timeout 20 s -> Paused. Kotlin solo mapea estado y refresca plan.
- C-06: capa Kotlin de referencia (2026-11-10): PlaybackController (singleton, StateFlows, cola
  gapless, refresh de plan en onRouteChanged) + PlayerStateUi (mapper con Suspended) +
  PlaybackService (MediaSession: Suspended->STATE_BUFFERING; notificación MediaStyle; focus;
  wakelock; becoming-noisy). TODO contacto JNI aislado en EngineBridge (PlaybackController.kt):
  si cambian los nombres de la fachada NativeAudioEngine, se ajusta SOLO ese adaptador.
  Archivos clave:
- cpp: AudioEngine.cpp/.h, OutputPlanner.h, Resampler.h, SampleConvert.h, RingBuffer.h,
  PcmSource.cpp/.h, jni_bridge.cpp, Log.h, CMakeLists.txt
- kotlin: audio/{OutputRouter,NativeAudioEngine,BitPerfectManager,AudioFocusManager}.kt,
  data/PlaybackController.kt, service/PlaybackService.kt,
  ui/{MainActivity,PlayerViewModel,PlayerStateUi}.kt, domain/Models.kt

## 4. Historial de sesiones
- 2026-11-09 | Fase 1 revisada (motor gapless FLAC/WAV, Service+MediaSession, Compose MVVM). Repo público.
- 2026-11-09 | Bugfix: OutputRouter.kt:78 `IntArray.mapNotNull` -> `.asSequence().mapNotNull(::mapEncoding)...`.
- 2026-11-10 | Fase 2 mejorada y pusheada (commits 483b558..2f5695c): dither TPDF, verificación buffer
  size + WARNING telemetría, @Volatile routeKey, mResamplerActive.
- 2026-11-10 | Prompt fundacional incorporado al contexto; AI_HANDOFF v2 (gap analysis + roadmap maestro).
- 2026-11-10 | Fase 7-parcial C++: auto-recuperación hot-plug (Suspended=6, frame de reanudación
  compensado por resampler, hilo recovery try_lock 800ms/timeout 20s, telemetría suspend/recover).
- 2026-11-10 | Kotlin hot-plug integrado como capa de referencia completa (v4): PlayerStateUi.kt nuevo;
  PlaybackController.kt (EngineBridge adapter, refresh plan en onRouteChanged, cola gapless, avisos
  telemetría); PlayerViewModel.kt (flows + Suspended); PlaybackService.kt (STATE_BUFFERING en 6,
  MediaStyle, focus, wakelock, noisy). Pendiente: reconciliar features extra previas si existían y
  validación hardware (pruebas 1–7).

## 5. Refinamientos pendientes de code review (NO bloqueantes)
- [ ] Dither TPDF en fromFloat() para targets enteros en rutas ya no bit-perfect.
- [ ] RNG más barato (xorshift64*) si underruns en gama baja a 384 kHz.
- [ ] mResamplerActive: exponer getter para UI ("SRC activo") o eliminar.
- [ ] mixerDevice (OutputRouter): confinar a un hilo o proteger con mutex.
- [ ] Higiene repo: .gitignore (.gradle/, .idea/, app/build/, build/, app/.cxx/, local.properties),
  `git rm -r --cached ...`, commits descriptivos, unificar branches.
- [ ] Reconciliar capa Kotlin de referencia con features previas del proyecto (si el service/viewmodel
  antiguos tenían extras: media buttons propios, cola personalizada, etc.).
- [ ] Validar hot-plug en hardware: pruebas 1–7 de §9 (PENDIENTES.md).

## 6. Deuda vs especificación original (gap analysis)
HECHO: motor FLAC/WAV gapless; bit-perfect USB DAC (exclusive/MMAP/mixer BP API34); resampler HQ;
dither TPDF; telemetría con warnings; ForegroundService/MediaSession/wakelock; UI básica Compose;
hot-plug: detección + auto-recuperación con reanudación en el mismo frame (C++ y Kotlin, pendiente validación).
PENDIENTE: ALAC y DSD/DoP; reporte codec Bluetooth (LDAC/aptX HD); escaneo biblioteca + Room;
MIR (BPM/key/waveform); DSP (EQ13/ReplayGain-LUFS/compresor); Automix/DJ; UI biblioteca/DJ; Doze completo.

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
- DD-06 Recovery hot-plug: hilo con mMutex.try_lock() (anti-deadlock con join bajo mMutex); frame de
  reanudación = mStartOffset + rendered*(src/out) (compensa resampler); ring descartado (se reanuda donde
  el usuario OYÓ); auto-recovery solo si reproducía; en pausa, play() recupera.
- DD-07 Aislamiento JNI: EngineBridge como único punto de contacto Kotlin->nativo (C-06).

## 9. Pruebas y observabilidad
- Logcat: HiFi / HiFi.Route / HiFi.Engine / HiFi.Ctrl (+ futuros HiFi.DSP, HiFi.MIX, HiFi.MIR).
- Líneas clave: "NEG ...", "Buffer configurado: ...", "Stream final: mode=...;buffer=...;neg=...",
  "WARNING: exclusive mode unavailable...", "SUSPEND: DAC desconectado; reanudacion preparada en frame N",
  "RECOVER OK: stream reabierto y reanudado tras K intento(s)", "RECOVER timeout ...",
  "plan: dev=... (mezclador ...)" (HiFi.Ctrl, refresh tras hot-plug).
- Telemetría (outputInfo): mode/excl/mmap/api/dev/rate/fmt/src/bits/resampled/attempts/why/buffer/neg
    + append: suspend=1;resumeFrame=N; recover=ok|timeout|cancel;attempts=K.
- UI: estado Suspended muestra "Esperando DAC…"; MediaSession en STATE_BUFFERING; aviso ámbar si
  bpWarning (WARNING exclusive); aviso si recover=timeout.
- Hardware: DAC KT02H20 / JM12; PCM 32-bit/384 kHz; pares gapless 10+11 y 20+21.
- Hot-plug (pruebas 1–7 en PENDIENTES.md): (1) reconexión <20 s reanuda en mismo frame y modo
  EXCL/MIXER_BP; (2) desconexión >20 s -> recover=timeout y Paused, play reanuda; (3) pausa durante
  suspensión cancela (recover=cancel); (4) seek en suspensión redefine punto; (5) desconexión en pausa
  no lanza recovery; (6) gapless con cola encolada sobrevive reconexión; (7) error no-disconnect
  mantiene comportamiento Error clásico.
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