package com.hifiplayer.audio

/** Convierte la telemetría del motor ("clave=valor;...") en la línea de estado que explica POR QUÉ se eligió la salida. */
object OutputTelemetry {
    fun parse(info: String): Map<String, String> = info.split(';').mapNotNull {
        val i = it.indexOf('=')
        if (i <= 0) null else it.substring(0, i) to it.substring(i + 1)
    }.toMap()

    fun line(info: String, deviceName: String?, mixerNote: String): String {
        if (info.isEmpty()) return ""
        val m = parse(info)
        val rate = m["rate"] ?: "?"
        val src = m["src"] ?: "?"
        val fmt = m["fmt"] ?: "?"
        val where = deviceName ?: "salida estándar"
        val resampled = m["resampled"] == "1"
        return when (m["mode"]) {
            "MIXER_BP" -> "Salida Bit-Perfect (mezclador Android 14+): $rate Hz / $fmt @ $where"
            "EXCL" ->
                if (!resampled) "Salida Bit-Perfect Directa ($rate Hz / $fmt @ $where)"
                else "Salida Directa Exclusiva con resampling $src→$rate Hz ($fmt @ $where) — NO bit-perfect"
            else -> {
                val why = m["why"].orEmpty().ifEmpty { "exclusivo no concedido" }
                val tries = m["attempts"]?.takeIf { it != "0" }?.let { ", $it intentos" }.orEmpty()
                "Salida compartida (mezclador de Android): $rate Hz / $fmt @ $where — $why$tries. $mixerNote"
            }
        }
    }
}
