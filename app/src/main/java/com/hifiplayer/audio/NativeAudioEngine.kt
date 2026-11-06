package com.hifiplayer.audio

/** Fachada JNI sobre el motor C++ (Oboe + dr_flac/dr_wav). Todas las llamadas son thread-safe en nativo. */
object NativeAudioEngine {
    init {
        System.loadLibrary("hifiengine")
        nCreate()
    }

    fun load(fd: Int) = nLoad(fd)
    fun queueNext(fd: Int) = nQueueNext(fd)
    fun play() = nPlay()
    fun pause() = nPause()
    fun stop() = nStop()
    fun seek(ms: Long) = nSeek(ms)
    fun positionMs() = nPositionMs()
    fun durationMs() = nDurationMs()
    fun state() = nState()
    fun transitions() = nTransitions()
    fun setBitPerfect(on: Boolean) = nSetBitPerfect(on)
    /** [sampleRate, channels, bitsPerSample] de la pista actual. */
    fun format(): IntArray = nFormat()
    fun describe(): String = nDescribe()
    fun release() = nDestroy()

    private external fun nCreate()
    private external fun nDestroy()
    private external fun nLoad(fd: Int): Boolean
    private external fun nQueueNext(fd: Int): Boolean
    private external fun nPlay(): Boolean
    private external fun nPause()
    private external fun nStop()
    private external fun nSeek(ms: Long): Boolean
    private external fun nPositionMs(): Long
    private external fun nDurationMs(): Long
    private external fun nState(): Int
    private external fun nTransitions(): Int
    private external fun nSetBitPerfect(on: Boolean)
    private external fun nFormat(): IntArray
    private external fun nDescribe(): String
}
