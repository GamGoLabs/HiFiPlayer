package com.hifiplayer.audio

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioMixerAttributes
import android.os.Build
import android.util.Log

/**
 * Android 14+ (API 34): pide al framework que el mixer sea BIT_PERFECT para el DAC USB activo.
 * - USB DAC: bit-perfect real si el dispositivo lo soporta.
 * - Bluetooth (LDAC/aptX): el codec ya es con pérdida -> nunca bit-perfect.
 * - Altavoz / 3.5mm: pasa por el mixer del SoC; no hay garantía.
 */
class BitPerfectManager(context: Context) {
    private val am = context.getSystemService(AudioManager::class.java)
    private val attrs = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_MEDIA)
        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
        .build()
    private var activeDevice: AudioDeviceInfo? = null

    fun apply(rate: Int, channels: Int, bits: Int): String {
        if (Build.VERSION.SDK_INT < 34) return "Android < 14: sin control de mixer bit-perfect"
        val dev = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).firstOrNull {
            it.type == AudioDeviceInfo.TYPE_USB_DEVICE || it.type == AudioDeviceInfo.TYPE_USB_HEADSET
        } ?: return "Sin DAC USB conectado: salida estándar"
        return applyApi34(dev, rate, channels, bits)
    }

    @androidx.annotation.RequiresApi(34)
    private fun applyApi34(dev: AudioDeviceInfo, rate: Int, channels: Int, bits: Int): String {
        val mask = if (channels == 1) AudioFormat.CHANNEL_OUT_MONO else AudioFormat.CHANNEL_OUT_STEREO
        // Debe coincidir con el formato que abre el motor nativo (I16 si <=16 bit, I32 si no).
        val enc = if (bits <= 16) AudioFormat.ENCODING_PCM_16BIT else AudioFormat.ENCODING_PCM_32BIT
        val supported = am.getSupportedMixerAttributes(dev)
        val match = supported.firstOrNull {
            it.mixerBehavior == AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT &&
                it.format.sampleRate == rate && it.format.encoding == enc && it.format.channelMask == mask
        }
        if (match == null) {
            Log.w(TAG, "DAC '${dev.productName}' sin modo bit-perfect para $rate Hz/enc=$enc")
            return "DAC '${dev.productName}': sin bit-perfect para $rate Hz/${bits}bit"
        }
        if (activeDevice != null && activeDevice?.id != dev.id) clear()
        val ok = am.setPreferredMixerAttributes(attrs, dev, match)
        if (ok) activeDevice = dev
        Log.d(TAG, "setPreferredMixerAttributes(${dev.productName}, $rate Hz) -> $ok")
        return if (ok) "BIT-PERFECT activo en '${dev.productName}'" else "No se pudo activar bit-perfect"
    }

    fun clear() {
        if (Build.VERSION.SDK_INT < 34) return
        activeDevice?.let { am.clearPreferredMixerAttributes(attrs, it) }
        activeDevice = null
    }

    private companion object { const val TAG = "HiFi.BitPerfect" }
}
