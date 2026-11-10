#pragma once
#include <oboe/Oboe.h>

#include <atomic>
#include <cstdarg>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "OutputPlanner.h"
#include "PcmSource.h"
#include "RingBuffer.h"

// Motor Hi-Fi:  [PcmSource] -> hilo decoder (+ resampler si hace falta) -> RingBuffer -> callback Oboe -> DAC
class AudioEngine final : public oboe::AudioStreamDataCallback,
                          public oboe::AudioStreamErrorCallback {
public:
    enum State : int { Idle = 0, Ready, Playing, Paused, Ended, Error, Suspended = 6 };

    AudioEngine();
    ~AudioEngine();

    bool load(int fd);                 // reemplaza la pista actual
    bool queueNext(int fd);            // siguiente pista para gapless
    bool play();                       // desde Suspended: fuerza recuperacion inmediata
    void pause();                      // desde Suspended: cancela recovery y queda en Paused (posicion conservada)
    void stop();
    bool seekMs(int64_t ms);           // permitido tambien en Suspended (redefine el punto de reanudacion)

    int64_t positionMs();
    int64_t durationMs();
    int state() const { return mState.load(); }
    int transitions();                 // nº de cambios gapless consumidos desde load()
    void setBitPerfect(bool on) { mBitPerfect.store(on); }
    // Ganancia lineal 0..1 (atenuacion). 1.0 = camino bit-exact, sin tocar ni una muestra.
    void setVolume(float v) { mTargetGain.store(v < 0.f ? 0.f : (v > 1.f ? 1.f : v)); }

    // Plan de salida (lo calcula Kotlin a partir del AudioDeviceInfo del DAC):
    void setOutputPlan(int deviceId, const std::vector<int>& rates, const std::vector<int>& formats,
                       int forcedRate, int forcedFmt);

    StreamInfo currentInfo();
    std::string describe();
    std::string outputInfo();          // telemetria "clave=valor;..." de la ultima apertura de stream

    // Oboe
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void* data, int32_t numFrames) override;
    void onErrorAfterClose(oboe::AudioStream*, oboe::Result) override;

private:
    struct Boundary { uint64_t atFrame; StreamInfo info; };      // atFrame en frames de SALIDA
    struct NegEntry { int rate; int fmt; bool noExclusive; };

    bool openStreamLocked();
    bool tryOpen(int rate, OutFmt fmt, bool exclusive, int deviceId,
                 std::shared_ptr<oboe::AudioStream>& out, oboe::Result& r);
    void stopLocked();
    void startDecoderLocked();
    void stopDecoderLocked();
    void decoderLoop();
    void prefill();
    void resetTimelineLocked(uint64_t startFrame);
    void syncBoundariesLocked();       // requiere mTimeMutex
    bool compatible(const StreamInfo& a, const StreamInfo& b) const;

    // hot-plug / auto-recuperacion (requieren mMutex salvo las atomicas)
    void handleDisconnectLocked();
    bool recoverNowLocked();
    void startRecoveryLocked();
    void stopRecoveryLocked();
    void recoveryLoop();
    void appendTel(const char* fmt, ...);

    std::mutex mMutex;                 // operaciones de control
    std::mutex mSrcMutex;              // mNext
    std::mutex mTimeMutex;             // timeline / info actual
    std::mutex mTelMutex;              // telemetria

    std::unique_ptr<PcmSource> mSource, mNext;
    std::shared_ptr<oboe::AudioStream> mStream;
    std::unique_ptr<RingBuffer> mRing;

    OutFmt mOutFmt = OutFmt::Float;
    uint32_t mChannels = 2, mFrameBytes = 8;
    std::atomic<uint32_t> mStreamRate{0};      // frecuencia REAL del stream (puede diferir de la de la pista)
    std::atomic<bool> mResamplerActive{false}; // indica si hay resampling activo

    std::thread mThread;
    std::atomic<bool> mRun{false}, mEof{false}, mBitPerfect{true};
    std::atomic<int> mState{Idle};
    std::atomic<uint64_t> mFramesRendered{0}, mUnderruns{0};   // frames de SALIDA
    uint64_t mFramesWritten = 0;       // solo hilo decoder (frames de SALIDA)
    std::atomic<float> mTargetGain{1.0f};
    float mCurGain = 1.0f;             // solo hilo de audio (y apertura del stream, con el stream parado)
    float mGainStep = 0.001f;          // incremento por frame (rampa ~80 ms, sin clics)

    // negociacion de salida
    int mDeviceId = 0;
    OutCaps mCaps;
    int mForcedRate = 0, mForcedFmt = -1;
    std::map<std::string, NegEntry> mNegCache;
    std::string mTelemetry;

    // hot-plug
    std::thread mRecoverThread;
    std::atomic<bool> mRecoverRun{false};
    bool mSuspended = false;           // mMutex
    bool mWasPlaying = false;          // mMutex
    uint64_t mResumeFrame = 0;         // mMutex, frames de ORIGEN de la pista actual
    int mRecoverAttempts = 0;          // solo hilo recovery

    // timeline (mTimeMutex)
    StreamInfo mCurInfo;
    uint64_t mOriginRendered = 0, mStartOffset = 0, mStartFrame = 0;   // mStartOffset/mStartFrame en frames de ORIGEN
    int mTransitions = 0;
    std::deque<Boundary> mBoundaries;
};