# HiFi Player — Fase 1 (Motor de reproducción)

Abrir la carpeta en Android Studio (Ladybug+), con NDK + CMake 3.22.1 instalados desde el SDK Manager.
Al primer build, CMake descarga `dr_flac.h` y `dr_wav.h` a `app/src/main/cpp/third_party/dr_libs/`.

Probar en dispositivo real: "Elegir archivos", seleccionar varios FLAC/WAV del mismo sample rate
para comprobar gapless. Filtrar logcat por `HiFi`.

Limitaciones Fase 1: solo FLAC/WAV; gapless solo con mismo sample rate/canales (y misma clase 16/24 bit en modo
bit-perfect); sin auto-recuperación al desconectar el DAC; sin DSP.
