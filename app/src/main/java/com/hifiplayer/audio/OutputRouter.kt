package com.hifiplayer.audio

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioMixerAttributes
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.annotation.RequiresApi

/**
 * Descubre el DAC USB, lee sus capacidades REALES (AudioDeviceInfo) y prepara el plan de salida para el motor nativo.
 *  - deviceId explícito: el stream se enruta al DAC, no al dispositivo por defecto.
 *  - capacidades (frecuencias / codificaciones) para que la negociación en C++ solo pruebe combinaciones plausibles.
 *  - Android 14+: busca un modo de mezclador BIT_PERFECT compatible con la pista (cualquier codificación sin pérdida,
 *    no solo PCM16/PCM32) y, si existe, lo activa.
 *  - Vigila enchufado/desenchufado (AudioDeviceCallback) para renegociar.
 * Códigos de formato (comunes con C++ OutFmt): 0=PCM_16, 1=PCM_24 empaquetado, 2=PCM_32, 3=FLOAT.
 */
class OutputRouter(context: Context, private val onRouteChanged: () -> Unit) {
    class Plan(
        val deviceId: Int,
        val deviceName: String?,
        val rates: IntArray,
        val formats: IntArray,
        val forcedRate: Int,
        val forcedFmt: Int,
        val mixerNote: String,
    )

    private val am = context.getSystemService(AudioManager::class.java)
    private val attrs = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_MEDIA)
        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
        .build()
    private var mixerDevice: AudioDeviceInfo? = null
    private var routeKey = 0
    private var loggedDevice = -1

    private val callback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>) { checkRoute() }
        override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>) { checkRoute() }
    }

    init {
        routeKey = usbOutput()?.id ?: 0
        am.registerAudioDeviceCallback(callback, Handler(Looper.getMainLooper()))
    }

    fun release() { am.unregisterAudioDeviceCallback(callback); clearMixer() }

    fun usbOutput(): AudioDeviceInfo? = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).firstOrNull {
        it.type == AudioDeviceInfo.TYPE_USB_DEVICE || it.type == AudioDeviceInfo.TYPE_USB_HEADSET
    }

    private fun checkRoute() {
        val k = usbOutput()?.id ?: 0
        if (k != routeKey) {
            Log.d(TAG, "ruta de salida cambió: dac $routeKey -> $k")
            routeKey = k
            onRouteChanged()
        }
    }

    fun plan(srcRate: Int, channels: Int, bits: Int, bitPerfect: Boolean): Plan {
        clearMixer()
        val dev = if (bitPerfect) usbOutput() else null
        if (dev == null) {
            return Plan(0, null, IntArray(0), IntArray(0), 0, -1,
                if (bitPerfect) "sin DAC USB conectado" else "bit-perfect desactivado")
        }
        val rates = dev.sampleRates
        val formats = dev.encodings.mapNotNull { mapEncoding(it) }.distinct().toIntArray()
        if (loggedDevice != dev.id) { loggedDevice = dev.id; logCaps(dev) }

        var forcedRate = 0
        var forcedFmt = -1
        var note = if (Build.VERSION.SDK_INT >= 34) "mezclador Android: sin modo bit-perfect compatible"
                   else "mezclador bit-perfect requiere Android 14+"
        if (Build.VERSION.SDK_INT >= 34) {
            tryMixer(dev, srcRate, channels, bits)?.let { (r, f) ->
                forcedRate = r; forcedFmt = f
                note = "mezclador bit-perfect $r Hz/${fmtLabel(f)}"
            }
        }
        return Plan(dev.id, dev.productName?.toString(), rates, formats, forcedRate, forcedFmt, note)
    }

    @RequiresApi(34)
    private fun tryMixer(dev: AudioDeviceInfo, rate: Int, channels: Int, bits: Int): Pair<Int, Int>? {
        val supported = am.getSupportedMixerAttributes(dev)
        if (loggedMixer != dev.id) {
            loggedMixer = dev.id
            if (supported.isEmpty()) Log.d(TAG, "mezclador: el DAC no declara ningún modo (lista vacía)")
            supported.forEach {
                Log.d(TAG, "mezclador: ${it.format.sampleRate} Hz ${encLabel(it.format.encoding)} " +
                    "máscara=${it.format.channelMask} comportamiento=" +
                    (if (it.mixerBehavior == AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT) "BIT_PERFECT" else "DEFAULT"))
            }
        }
        val pref = when {
            bits <= 16 -> intArrayOf(0, 2, 1, 3)    // exacto primero, luego contenedores mas anchos (todos sin perdida)
            bits <= 24 -> intArrayOf(2, 1, 3)
            else -> intArrayOf(2)
        }
        val matches = supported.filter {
            it.mixerBehavior == AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT &&
                it.format.sampleRate == rate && it.format.channelMask.countOneBits() == channels
        }
        for (f in pref) {
            val a = matches.firstOrNull { mapEncoding(it.format.encoding) == f } ?: continue
            if (am.setPreferredMixerAttributes(attrs, dev, a)) {
                mixerDevice = dev
                Log.d(TAG, "mezclador BIT_PERFECT activado: $rate Hz ${fmtLabel(f)} en '${dev.productName}'")
                return rate to f
            }
        }
        return null
    }

    fun clearMixer() {
        if (Build.VERSION.SDK_INT < 34) return
        mixerDevice?.let { runCatching { am.clearPreferredMixerAttributes(attrs, it) } }
        mixerDevice = null
    }

    private fun logCaps(dev: AudioDeviceInfo) {
        Log.d(TAG, "DAC id=${dev.id} nombre='${dev.productName}' tipo=${dev.type} " +
            "frecuencias=${dev.sampleRates.joinToString()} " +
            "codificaciones=${dev.encodings.joinToString { encLabel(it) }} " +
            "canales=${dev.channelCounts.joinToString()}")
    }

    private var loggedMixer = -1

    private fun mapEncoding(e: Int): Int? = when (e) {
        AudioFormat.ENCODING_PCM_16BIT -> 0
        AudioFormat.ENCODING_PCM_24BIT_PACKED -> 1
        AudioFormat.ENCODING_PCM_32BIT -> 2
        AudioFormat.ENCODING_PCM_FLOAT -> 3
        else -> null
    }

    private fun encLabel(e: Int) = when (e) {
        AudioFormat.ENCODING_PCM_16BIT -> "PCM_16"
        AudioFormat.ENCODING_PCM_24BIT_PACKED -> "PCM_24"
        AudioFormat.ENCODING_PCM_32BIT -> "PCM_32"
        AudioFormat.ENCODING_PCM_FLOAT -> "PCM_FLOAT"
        else -> "enc$e"
    }

    private fun fmtLabel(f: Int) = when (f) { 0 -> "PCM_16"; 1 -> "PCM_24"; 2 -> "PCM_32"; else -> "PCM_FLOAT" }

    private companion object { const val TAG = "HiFi.Route" }
}
