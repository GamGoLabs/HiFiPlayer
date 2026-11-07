#pragma once
#include <oboe/Oboe.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "PcmSource.h"
#include "RingBuffer.h"

// Motor Hi-Fi:  [PcmSource] -> hilo decoder -> RingBuffer -> callback Oboe -> DAC/altavoz
// Punto de enganche futuro del DSP: entre decoder y ring (hilo decoder, float) -> Fase 3.
class AudioEngine final : public oboe::AudioStreamDataCallback,
                          public oboe::AudioStreamErrorCallback {
public:
    enum State : int { Idle = 0, Ready, Playing, Paused, Ended, Error };

    AudioEngine();
    ~AudioEngine();

    bool load(int fd);                 // reemplaza la pista actual
    bool queueNext(int fd);            // siguiente pista para gapless
    bool play();
    void pause();
    void stop();
    bool seekMs(int64_t ms);

    int64_t positionMs();
    int64_t durationMs();
    int state() const { return mState.load(); }
    int transitions();                 // nº de cambios gapless consumidos desde load()
    void setBitPerfect(bool on) { mBitPerfect.store(on); }
    // Ganancia lineal 0..1 (atenuacion/ducking). 1.0 = camino bit-exact, sin tocar ni una muestra.
    void setVolume(float v) { mTargetGain.store(v < 0.f ? 0.f : (v > 1.f ? 1.f : v)); }
    StreamInfo currentInfo();
    std::string describe();

    // Oboe
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void* data, int32_t numFrames) override;
    void onErrorAfterClose(oboe::AudioStream*, oboe::Result) override;

private:
    struct Boundary { uint64_t atFrame; StreamInfo info; };

    bool openStreamLocked();
    void stopLocked();
    void startDecoderLocked();
    void stopDecoderLocked();
    void decoderLoop();
    void applyGain(void* data, size_t frames);   // solo hilo de audio
    void prefill();
    void resetTimelineLocked(uint64_t startFrame);
    void syncBoundariesLocked();       // requiere mTimeMutex
    bool compatible(const StreamInfo& a, const StreamInfo& b) const;
    oboe::AudioFormat chooseFormat(const StreamInfo& i) const;

    std::mutex mMutex;                 // operaciones de control
    std::mutex mSrcMutex;              // mNext
    std::mutex mTimeMutex;             // timeline / info actual

    std::unique_ptr<PcmSource> mSource, mNext;
    std::shared_ptr<oboe::AudioStream> mStream;
    std::unique_ptr<RingBuffer> mRing;

    oboe::AudioFormat mOutFormat = oboe::AudioFormat::Float;
    uint32_t mChannels = 2, mFrameBytes = 8;

    std::thread mThread;
    std::atomic<bool> mRun{false}, mEof{false}, mBitPerfect{true};
    std::atomic<int> mState{Idle};
    std::atomic<uint64_t> mFramesRendered{0}, mUnderruns{0};
    std::atomic<float> mTargetGain{1.0f};
    float mCurGain = 1.0f;        // solo hilo de audio (y apertura del stream, con el stream parado)
    float mGainStep = 0.001f;     // incremento por frame (rampa ~80 ms, sin clics)
    uint64_t mFramesWritten = 0;       // solo hilo decoder

    // timeline (mTimeMutex)
    StreamInfo mCurInfo;
    uint64_t mOriginRendered = 0, mStartOffset = 0, mStartFrame = 0;
    int mTransitions = 0;
    std::deque<Boundary> mBoundaries;
};
