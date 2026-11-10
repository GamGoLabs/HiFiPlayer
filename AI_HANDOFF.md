# AI_HANDOFF.md — Contexto del proyecto para asistentes de IA
<!-- Mantener actualizado al cierre de cada sesión. Última actualización: 2026-11-10 (v2: incluye prompt fundacional) -->

## 0. Cómo usar este archivo (leer primero)
1. Lee este archivo completo y `PENDIENTES.md` (protocolo de pruebas en hardware).
2. Lee los archivos clave listados en §3.
3. Pregunta al usuario qué ítem del roadmap (§7) o de los pendientes (§5/§6) se aborda hoy.
4. Trabaja de forma ITERATIVA y por módulos (regla del prompt fundacional: NUNCA generar toda la app en una sola respuesta).
5. Al terminar: actualiza §4 (historial) y §5/§6/§7 de este archivo y commitea con mensaje descriptivo.

## 1. Visión original (resumen del prompt fundacional — ver Apéndice A)
App Android de reproducción Hi-Fi para audiófilos + DJ/Automix inteligente.
Roles exigidos al asistente: Dev Senior Android + Ingeniero de Audio (DSP) + Especialista C++/NDK.
Alcance completo comprometido:
- Motor: FLAC/WAV/ALAC/DSD lossless; salida bit-perfect (bypass mixer Android); gapless;
  salidas: altavoz, Bluetooth (LDAC/aptX HD), AUX 3.5mm, USB-C DAC.
- Biblioteca: escaneo local profundo, organización por carpetas/géneros, reproducir carpeta o biblioteca.
- MIR: análisis en segundo plano de BPM, tonalidad (key) y forma de onda, con persistencia.
- DSP: EQ 13 bandas, ReplayGain/LUFS con pre-amp, compresor (Threshold/Ratio/Attack/Release).
- Automix: Modo Carpeta (coherencia género/BPM) y Modo Biblioteca (transiciones adaptativas según
  diferencia de BPM); comportamiento DJ real: beatmatching, alineación de frases, EQ dinámica en
  crossfade, efectos (echo out, filtros HP/LP, frenado de vinilo).
- Rendimiento: resistencia a Doze; logs exhaustivos en capas DSP y Automix.
  Metodología: fases iterativas por módulos (Fase 1 = motor; luego escaneo/MIR, DSP, Automix, UI).

## 2. Decisiones técnicas ya adoptadas (con rationale)
- DT-01 Oboe/AAudio + motor C++ propio (vs ExoPlayer/Superpowered/FMOD): control total del path
  bit-perfect, sharing mode y buffers. ExoPlayer descartado (resamplea/mezcla siempre).
- DT-02 Decodificación con dr_libs (dr_flac/dr_wav) en C++; I/O por file descriptor (dup(fd))
  => compatible con SAF/Scoped Storage sin permisos especiales.
- DT-03 Resampler propio Kaiser (~100 dB, polifásico, streaming-safe) en vez de SRC de Android.
- DT-04 Conversión de formatos y ganancia en SampleConvert.h (header-only, testeable en PC);
  applyGain con target=1.0 no toca muestras (camino bit-perfect garantizado).
- DT-05 Dithering TPDF en conversión int32->float (implementado 2026-11-10).
- DT-06 Negociación de salida en cascada con verificación del modo REAL (Exclusive/MMAP) y
  cache mNegCache por (deviceId|srcRate|bits|channels); mezclador BIT_PERFECT API 34+
  (setPreferredMixerAttributes) gestionado desde Kotlin (OutputRouter).
- DT-07 Códigos de formato compartidos Kotlin<->C++ (OutFmt): 0=PCM_16, 1=PCM_24 packed,
  2=PCM_32, 3=FLOAT.
- DT-08 Tags de log: HiFi (general), HiFi.Route (OutputRouter), HiFi.Engine (nativo).
  Futuros: HiFi.DSP, HiFi.MIX, HiFi.MIR (requisito del prompt fundacional: logs exhaustivos).

## 3. Arquitectura actual y CONTRATOS QUE NO SE ROMPEN
Flujo: PcmSource(fd) -> hilo decoder (+Resampler si hace falta) -> RingBuffer SPSC ->
callback Oboe (tiempo real) -> DAC.
- C-01: el callback onAudioReady NO hace locks, malloc ni logs (solo RingBuffer.read + applyGain).
- C-02: bit-perfect = camino sin DSP ni conversión; cualquier feature nueva debe respetar el
  modo PURE o declararse explícitamente como modo PROCESSED (ver DD-01 en §8).
- C-03: timeline gapless por mBoundaries (atFrame en frames de SALIDA); positionMs() extrapola
  con mFramesRendered (no spamea binder).
- C-04: hot-plug: AudioDeviceCallback (@Volatile routeKey) dispara renegociación; onErrorAfterClose
  marca Error y Kotlin decide reintento.
  Archivos clave:
- cpp: AudioEngine.cpp/.h, OutputPlanner.h, Resampler.h, SampleConvert.h, RingBuffer.h,
  PcmSource.cpp/.h, jni_bridge.cpp, Log.h, CMakeLists.txt
- kotlin: audio/{OutputRouter,NativeAudioEngine,BitPerfectManager,AudioFocusManager}.kt,
  service/PlaybackService.kt, data/PlaybackController.kt, ui/{MainActivity,PlayerViewModel}.kt,
  domain/Models.kt

## 4. Historial de sesiones
- 2026-11-09 | Fase 1 revisada: motor gapless FLAC/WAV, ForegroundService+MediaSession, Compose MVVM.
  Repo pasó de privado a público.
- 2026-11-09 | Bugfix compilación: OutputRouter.kt:78 `IntArray.mapNotNull` no existe ->
  `.asSequence().mapNotNull(::mapEncoding).distinct().toList().toIntArray()`.
- 2026-11-10 | Fase 2 (negociación DAC) revisada y mejorada; pusheado en commits 483b558..2f5695c:
  dithering TPDF (SampleConvert.h), verificación buffer size + WARNING telemetría (AudioEngine.cpp),
  @Volatile routeKey (OutputRouter.kt), mResamplerActive (AudioEngine.h/.cpp).
  NOTA OPERATIVA: raw.githubusercontent cachea; verificar cambios con `?nocache=` o API de commits
  antes de afirmar que "no hay cambios" (ya causó una falsa alarma este día).
- 2026-11-10 | Se incorpora el prompt fundacional al contexto y se genera AI_HANDOFF.md v2.

## 5. Refinamientos pendientes de code review (NO bloqueantes)
- [ ] Dither TPDF en fromFloat() para targets enteros (I16/I24/I32) en rutas ya no bit-perfect
  (post-resampler / fuentes float). El dither actual int32->Float es inaudible/inofensivo.
- [ ] RNG más barato (xorshift64*) si hay underruns en gama baja a 384 kHz.
- [ ] mResamplerActive: exponer getter para UI ("SRC activo") o eliminar.
- [ ] mixerDevice (OutputRouter): confinar a un hilo o proteger con mutex.
- [ ] Higiene repo: .gitignore (.gradle/, .idea/, app/build/, build/, app/.cxx/, local.properties),
  `git rm -r --cached ...`, mensajes de commit descriptivos, unificar branches
  (master/main/MasterDaCv1).

## 6. Deuda vs especificación original (gap analysis)
HECHO: motor FLAC/WAV gapless; bit-perfect USB DAC (exclusive/MMAP/mixer BP API34); resampler HQ;
telemetría; ForegroundService/MediaSession/wakelock; UI básica Compose; hot-plug detect (renegocia).
PENDIENTE: ALAC y DSD/DoP; reporte de codec Bluetooth (LDAC/aptX HD); escaneo de biblioteca +
persistencia (Room); MIR (BPM/key/waveform); DSP (EQ13/ReplayGain-LUFS/compresor); Automix/DJ
completo; auto-recuperación hot-plug con reanudación en el mismo frame; UI biblioteca/DJ;
validación Doze completa (ver PENDIENTES.md).

## 7. Roadmap maestro (alineado al prompt fundacional)
FASE 3 — Biblioteca y persistencia: escaneo SAF tree + MediaStore; Room (tracks, playlists,
analysis); navegación por carpetas/géneros; "reproducir carpeta"; cola persistida.
FASE 4 — MIR offline: WorkManager + kissfft/pffft; BPM (spectral flux + comb filter), key
(chroma + perfiles Krumhansl-Schmuckler), waveform (picos min/max por ventana), ReplayGain/LUFS
(libebur128); resultados en Room; reanálisis incremental.
FASE 5 — DSP (modo PROCESSED, ver DD-01): cadena en hilo decoder: EQ 13 bandas (cascada biquad,
fórmulas RBJ), compresor con lookahead (Threshold/Ratio/Attack/Release), ganancia ReplayGain +
pre-amp integrada en applyGain; bypass total = modo PURE.
FASE 6 — Automix/DJ: doble deck (2x PcmSource+DSP) -> etapa Mixer -> RingBuffer único; crossfader;
beatmatching (pitch via resampler de ratio dinámico), alineación de frases (beat grids de Fase 4),
EQ dinámica en crossfade, efectos: echo out, HP/LP, frenado de vinilo (envolvente de pitch);
Modo Carpeta y Modo Biblioteca.
FASE 7 — Formatos y salidas avanzadas: ALAC; DSD nativo/DoP (detección de capacidad del DAC);
reporte codec Bluetooth; auto-recuperación hot-plug (reabrir y reanudar en el mismo frame).
FASE 8 — UI final y endurecimiento: biblioteca Compose, vista DJ (decks + waveforms), ajustes,
Doze completo, pruebas de instrumento + protocolo de hardware (PENDIENTES.md).
Criterio de cierre de cada fase: compila, pruebas logcat OK, y actualización de §4/§6 de este archivo.

## 8. Decisiones de diseño emergentes (para fases futuras)
- DD-01 DOS caminos de audio: PURE (bit-perfect, sin DSP; motor actual) y PROCESSED (DSP/Automix).
  La selección es explícita por sesión y visible en UI/telemetría; nunca se degrada PURE en silencio.
- DD-02 Todo DSP vive en el hilo decoder (antes del RingBuffer); NUNCA en el callback Oboe (C-01).
- DD-03 Automix implica etapa de mezcla post-decoder: dos cadenas decodificadas se suman en un
  Mixer stage que alimenta el ÚNICO RingBuffer (no duplicar streams Oboe).
- DD-04 MIR offline y persistente (Room), nunca en línea de reproducción; el automix consume
  metadatos ya analizados.
- DD-05 Permisos: preferir SAF+FD (ya soportado por el motor); MANAGE_EXTERNAL_STORAGE solo si es
  imprescindible para escaneo masivo (riesgo de política Play Store).

## 9. Pruebas y observabilidad
- Logcat: tags HiFi / HiFi.Route / HiFi.Engine (+ futuros HiFi.DSP, HiFi.MIX, HiFi.MIR).
- Líneas clave actuales: "NEG ...", "Buffer configurado: ...", "Stream final: mode=...;buffer=...;neg=...",
  "WARNING: exclusive mode unavailable...".
- Hardware: DAC KT02H20 / JM12; archivos PCM 32-bit/384 kHz; pares gapless 10+11 y 20+21.
- Casos: hot-plug en reproducción, Doze 10-15 min, llamada entrante, otro reproductor,
  cambio de carpeta con formatos mixtos. Detalle completo en PENDIENTES.md.

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