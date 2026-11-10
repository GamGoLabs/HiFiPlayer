# Pendientes de prueba y decisiones abiertas

## Pruebas en el dispositivo
- [ ] **DAC KT02H20 / JM12 — leer el resultado de la negociación** (Logcat `tag:HiFi`): líneas `HiFi.Route` (capacidades reales del DAC y lista
      de modos de mezclador) y `HiFi.Engine NEG ...` / `Stream final: ...` (qué combinación concedió el sistema y por qué).
- [ ] **DAC: PCM 32 bit / 384 kHz y DSD128** (indicado en la etiqueta del DAC). Probar con los archivos 04 (192 kHz) y 05/06 (32 bit)
      del pack de pruebas. DSD no está implementado (ver "Fases futuras"): hoy solo se puede probar PCM.
- [ ] Pack de pruebas: archivos 01–08 (formatos), 10+11 (gapless), 20+21 (formatos distintos).
- [ ] Segundo plano con pantalla apagada 10–15 min (Doze).
- [ ] Llamada (timbre → atenúa; contestada → pausa), otro reproductor/vídeo (pausa), desconectar auriculares (pausa).
- [ ] Desenchufar y volver a enchufar el DAC durante la reproducción (debe renegociar y continuar en la misma posición).

## Decisiones abiertas
- **Acceso USB directo (driver UAC propio con libusb)**: la única vía para bit-perfect/DSD nativo si el sistema no concede modo
  exclusivo para el DAC. Es un motor aparte (descriptores UAC1/UAC2, endpoints isócronos, reloj/feedback). El permiso `UsbManager`
  por sí solo NO cambia lo que concede AAudio; solo sirve para este driver propio.
- Pre-resamplear a la frecuencia del mezclador cuando solo hay modo compartido (evitar el remuestreo de Android).
