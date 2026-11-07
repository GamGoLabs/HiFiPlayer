#include "AudioEngine.h"
#include "Log.h"

#include <chrono>
#include <cstring>
#include <vector>

using namespace std::chrono_literals;

static void convert(const int32_t* in, size_t samples, oboe::AudioFormat f, uint8_t* out) {
    switch (f) {
        case oboe::AudioFormat::I16: {
            auto* o = reinterpret_cast<int16_t*>(out);
            for (size_t i = 0; i < samples; ++i) o[i] = static_cast<int16_t>(in[i] >> 16);   // exacto
            break;
        }
        case oboe::AudioFormat::I32:
            std::memcpy(out, in, samples * sizeof(int32_t));                                 // bit-exact
            break;
        default: {
            auto* o = reinterpret_cast<float*>(out);
            for (size_t i = 0; i < samples; ++i) o[i] = static_cast<float>(in[i]) * (1.0f / 2147483648.0f);
        }
    }
}

AudioEngine::AudioEngine() { LOGD("AudioEngine creado"); }
AudioEngine::~AudioEngine() { std::lock_guard<std::mutex> g(mMutex); stopLocked(); }

oboe::AudioFormat AudioEngine::chooseFormat(const StreamInfo& i) const {
    if (!mBitPerfect.load()) return oboe::AudioFormat::Float;
    return i.bitsPerSample <= 16 ? oboe::AudioFormat::I16 : oboe::AudioFormat::I32;
}

bool AudioEngine::compatible(const StreamInfo& a, const StreamInfo& b) const {
    if (a.sampleRate != b.sampleRate || a.channels != b.channels) return false;
    if (mBitPerfect.load() && ((a.bitsPerSample <= 16) != (b.bitsPerSample <= 16))) return false;
    return true;
}

// ---------------------------------------------------------------- control

bool AudioEngine::load(int fd) {
    std::lock_guard<std::mutex> g(mMutex);
    stopLocked();
    std::string err;
    auto src = PcmSource::openFd(fd, err);
    if (!src) { mState = Error; return false; }
    {
        std::lock_guard<std::mutex> s(mSrcMutex);
        mNext.reset();
    }
    mSource = std::move(src);
    {
        std::lock_guard<std::mutex> t(mTimeMutex);
        mCurInfo = mSource->info();
        mTransitions = 0;
        mBoundaries.clear();
        mFramesRendered = 0; mOriginRendered = 0; mStartOffset = 0; mStartFrame = 0;
    }
    mState = Ready;
    LOGD("load OK (bitPerfect=%d)", (int)mBitPerfect.load());
    return true;
}

bool AudioEngine::queueNext(int fd) {
    std::string err;
    auto src = PcmSource::openFd(fd, err);
    if (!src) return false;
    std::lock_guard<std::mutex> s(mSrcMutex);
    mNext = std::move(src);
    LOGD("queueNext listo para gapless");
    return true;
}

bool AudioEngine::openStreamLocked() {
    if (mStream) return true;
    StreamInfo in = currentInfo();
    mChannels = in.channels;

    auto build = [&](oboe::AudioFormat fmt) {
        mOutFormat = fmt;
        mFrameBytes = mChannels * (fmt == oboe::AudioFormat::I16 ? 2 : 4);
        oboe::AudioStreamBuilder b;
        b.setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)    // habilita MMAP/exclusive si el HAL lo permite
            ->setSharingMode(oboe::SharingMode::Exclusive)
            ->setUsage(oboe::Usage::Media)
            ->setContentType(oboe::ContentType::Music)
            ->setFormat(fmt)
            ->setChannelCount(mChannels)
            ->setSampleRate(in.sampleRate)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::None)
            ->setFormatConversionAllowed(false)
            ->setChannelConversionAllowed(false)
            ->setDataCallback(this)
            ->setErrorCallback(this);
        return b.openStream(mStream);
    };

    oboe::AudioFormat want = chooseFormat(in);
    oboe::Result r = build(want);
    if (r != oboe::Result::OK && want != oboe::AudioFormat::Float) {
        LOGW("Formato nativo %s rechazado (%s). Fallback a Float (NO bit-perfect)",
             oboe::convertToText(want), oboe::convertToText(r));
        mStream.reset();
        r = build(oboe::AudioFormat::Float);
    }
    if (r != oboe::Result::OK) { LOGE("openStream fallo: %s", oboe::convertToText(r)); mStream.reset(); return false; }

    mStream->setBufferSizeInFrames(mStream->getFramesPerBurst() * 8);   // musica: priorizar estabilidad
    mRing = std::make_unique<RingBuffer>(static_cast<size_t>(in.sampleRate) * mFrameBytes);   // ~1 s
    mGainStep = 1.0f / (0.08f * static_cast<float>(in.sampleRate));   // rampa completa 0->1 en ~80 ms
    mCurGain = mTargetGain.load();   // si se abre atenuado, empezar ya atenuado
    LOGD("Stream: api=%s %d Hz fmt=%s sharing=%s perf=%s burst=%d",
         oboe::convertToText(mStream->getAudioApi()), mStream->getSampleRate(),
         oboe::convertToText(mStream->getFormat()), oboe::convertToText(mStream->getSharingMode()),
         oboe::convertToText(mStream->getPerformanceMode()), mStream->getFramesPerBurst());
    if (mStream->getSampleRate() != (int32_t)in.sampleRate)
        LOGW("El dispositivo NO acepta %u Hz (abrio %d Hz): hay resampling, no es bit-perfect",
             in.sampleRate, mStream->getSampleRate());
    return true;
}

void AudioEngine::resetTimelineLocked(uint64_t startFrame) {
    mRing->reset();
    mFramesWritten = 0;
    mFramesRendered = 0;
    std::lock_guard<std::mutex> t(mTimeMutex);
    mOriginRendered = 0;
    mStartOffset = startFrame;
    mBoundaries.clear();
}

bool AudioEngine::play() {
    std::lock_guard<std::mutex> g(mMutex);
    int s = mState.load();
    if (s == Playing) return true;
    if (s == Paused) {
        if (mStream) mStream->requestStart();
        mState = Playing;
        return true;
    }
    if (!mSource || s == Idle || s == Error) return false;
    if (s == Ended) { stopLocked(); mSource->seek(0); mStartFrame = 0; }

    if (!openStreamLocked()) { mState = Error; return false; }
    resetTimelineLocked(mStartFrame);
    startDecoderLocked();
    prefill();
    auto r = mStream->requestStart();
    if (r != oboe::Result::OK) { LOGE("requestStart: %s", oboe::convertToText(r)); mState = Error; return false; }
    mState = Playing;
    return true;
}

void AudioEngine::pause() {
    std::lock_guard<std::mutex> g(mMutex);
    if (mState == Playing && mStream) { mStream->requestPause(); mState = Paused; }
}

void AudioEngine::stop() {
    std::lock_guard<std::mutex> g(mMutex);
    if (!mSource) return;
    stopLocked();
    { std::lock_guard<std::mutex> s(mSrcMutex); mNext.reset(); }
    mSource->seek(0);
    mStartFrame = 0;
    {
        std::lock_guard<std::mutex> t(mTimeMutex);
        mStartOffset = 0; mOriginRendered = 0; mBoundaries.clear();
    }
    mFramesRendered = 0;
    mState = Ready;
}

bool AudioEngine::seekMs(int64_t ms) {
    std::lock_guard<std::mutex> g(mMutex);
    if (!mSource) return false;
    int s = mState.load();
    if (s == Error || s == Idle) return false;

    bool resume = (s == Playing);
    bool streaming = (s == Playing || s == Paused || s == Ended);
    if (streaming) { stopDecoderLocked(); if (mStream) mStream->stop(); }

    StreamInfo info;
    {   // Limitacion conocida: si el decoder ya paso a la pista siguiente, el seek aplica a esa (v. roadmap)
        std::lock_guard<std::mutex> t(mTimeMutex);
        if (!mBoundaries.empty()) {
            LOGW("Seek con transicion gapless pendiente: se aplica a la pista siguiente");
            mTransitions += (int)mBoundaries.size();
            mCurInfo = mBoundaries.back().info;
            mBoundaries.clear();
        }
        info = mCurInfo;
    }
    uint64_t target = (uint64_t)(ms < 0 ? 0 : ms) * info.sampleRate / 1000;
    if (info.totalFrames && target >= info.totalFrames) target = info.totalFrames - 1;
    if (!mSource->seek(target)) { LOGE("seek fallo"); return false; }
    LOGD("seek -> %lld ms (frame %llu)", (long long)ms, (unsigned long long)target);

    mStartFrame = target;
    if (!streaming) { std::lock_guard<std::mutex> t(mTimeMutex); mStartOffset = target; return true; }

    resetTimelineLocked(target);
    startDecoderLocked();
    if (resume) { prefill(); mStream->requestStart(); mState = Playing; }
    else mState = Paused;
    return true;
}

void AudioEngine::stopDecoderLocked() {
    mRun = false;
    if (mThread.joinable()) mThread.join();
}

void AudioEngine::startDecoderLocked() {
    stopDecoderLocked();
    mRun = true;
    mEof = false;
    mThread = std::thread(&AudioEngine::decoderLoop, this);
}

void AudioEngine::stopLocked() {
    stopDecoderLocked();
    if (mStream) { mStream->stop(); mStream->close(); mStream.reset(); }
}

void AudioEngine::prefill() {
    auto deadline = std::chrono::steady_clock::now() + 300ms;
    while (mRing->readable() < mRing->capacity() / 4 && !mEof.load() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
}

// ---------------------------------------------------------------- decoder thread

void AudioEngine::decoderLoop() {
    constexpr size_t kChunk = 4096;
    std::vector<int32_t> pcm(kChunk * mChannels);
    std::vector<uint8_t> out(kChunk * mFrameBytes);
    uint64_t lastUnderruns = 0;
    uint64_t srcPos = mStartFrame;   // frame alcanzado dentro del archivo actual
    LOGD("decoder: inicio (desde frame %llu)", (unsigned long long)srcPos);

    while (mRun.load()) {
        uint64_t u = mUnderruns.load();
        if (u != lastUnderruns) { LOGW("underruns acumulados: %llu", (unsigned long long)u); lastUnderruns = u; }

        if (mRing->writable() < out.size()) { std::this_thread::sleep_for(5ms); continue; }

        uint64_t n = mSource->readFrames(pcm.data(), kChunk);
        if (n == 0) {
            const uint64_t declared = mSource->info().totalFrames;
            if (declared && srcPos != declared)
                LOGW("EOF en frame %llu pero la cabecera declara %llu (dif %lld frames = %.1f s): "
                     "archivo truncado, cabecera erronea o error de decodificacion",
                     (unsigned long long)srcPos, (unsigned long long)declared,
                     (long long)declared - (long long)srcPos,
                     (double)((long long)declared - (long long)srcPos) / mSource->info().sampleRate);
            else
                LOGD("EOF correcto en frame %llu", (unsigned long long)srcPos);
            std::unique_ptr<PcmSource> next;
            {
                std::lock_guard<std::mutex> s(mSrcMutex);
                if (mNext && compatible(mNext->info(), mSource->info())) next = std::move(mNext);
                else if (mNext) LOGW("Siguiente pista con formato distinto: sin gapless (se recarga al terminar)");
            }
            if (next) {
                mSource = std::move(next);
                srcPos = 0;
                std::lock_guard<std::mutex> t(mTimeMutex);
                mBoundaries.push_back({mFramesWritten, mSource->info()});
                LOGD("decoder: gapless en frame %llu", (unsigned long long)mFramesWritten);
                continue;
            }
            LOGD("decoder: EOF");
            mEof.store(true, std::memory_order_release);
            break;
        }
        convert(pcm.data(), n * mChannels, mOutFormat, out.data());
        mRing->write(out.data(), n * mFrameBytes);
        mFramesWritten += n;
        srcPos += n;
    }
    LOGD("decoder: fin");
}

// ---------------------------------------------------------------- callback (tiempo real: sin logs, sin locks, sin malloc)

// Tiempo real: sin locks, logs ni malloc. A volumen pleno NO modifica nada (bit-perfect intacto).
void AudioEngine::applyGain(void* data, size_t frames) {
    const float target = mTargetGain.load(std::memory_order_relaxed);
    float g = mCurGain;
    if (g == target && target == 1.0f) return;
    const float step = mGainStep;
    const uint32_t ch = mChannels;
    auto next = [&]() {
        if (g < target) { g += step; if (g > target) g = target; }
        else if (g > target) { g -= step; if (g < target) g = target; }
    };
    switch (mOutFormat) {
        case oboe::AudioFormat::I16: {
            auto* p = static_cast<int16_t*>(data);
            for (size_t f = 0; f < frames; ++f) {
                next();
                for (uint32_t c = 0; c < ch; ++c, ++p) *p = static_cast<int16_t>(static_cast<float>(*p) * g);
            }
            break;
        }
        case oboe::AudioFormat::I32: {
            auto* p = static_cast<int32_t*>(data);
            for (size_t f = 0; f < frames; ++f) {
                next();
                for (uint32_t c = 0; c < ch; ++c, ++p) *p = static_cast<int32_t>(static_cast<double>(*p) * g);
            }
            break;
        }
        default: {
            auto* p = static_cast<float*>(data);
            for (size_t f = 0; f < frames; ++f) {
                next();
                for (uint32_t c = 0; c < ch; ++c, ++p) *p *= g;
            }
        }
    }
    mCurGain = g;
}

oboe::DataCallbackResult AudioEngine::onAudioReady(oboe::AudioStream*, void* data, int32_t numFrames) {
    const size_t want = static_cast<size_t>(numFrames) * mFrameBytes;
    auto* dst = static_cast<uint8_t*>(data);
    size_t got = mRing->read(dst, want);
    mFramesRendered.fetch_add(got / mFrameBytes, std::memory_order_relaxed);
    if (got) applyGain(data, got / mFrameBytes);
    if (got < want) {
        std::memset(dst + got, 0, want - got);
        if (mEof.load(std::memory_order_acquire) && mRing->readable() == 0) {
            mState = Ended;
            return oboe::DataCallbackResult::Stop;
        }
        mUnderruns.fetch_add(1, std::memory_order_relaxed);
    }
    return oboe::DataCallbackResult::Continue;
}

void AudioEngine::onErrorAfterClose(oboe::AudioStream*, oboe::Result r) {
    LOGE("Stream cerrado por error: %s (DAC desconectado?)", oboe::convertToText(r));
    mState = Error;   // Kotlin decide si reintenta (Fase posterior: auto-recover)
}

// ---------------------------------------------------------------- consultas

void AudioEngine::syncBoundariesLocked() {
    uint64_t rendered = mFramesRendered.load();
    while (!mBoundaries.empty() && rendered >= mBoundaries.front().atFrame) {
        mOriginRendered = mBoundaries.front().atFrame;
        mStartOffset = 0;
        mCurInfo = mBoundaries.front().info;
        ++mTransitions;
        mBoundaries.pop_front();
        LOGD("transicion gapless renderizada (#%d)", mTransitions);
    }
}

int64_t AudioEngine::positionMs() {
    std::lock_guard<std::mutex> t(mTimeMutex);
    syncBoundariesLocked();
    if (!mCurInfo.sampleRate) return 0;
    int64_t frames = (int64_t)mStartOffset + (int64_t)(mFramesRendered.load() - mOriginRendered);
    return frames * 1000 / mCurInfo.sampleRate;
}

int64_t AudioEngine::durationMs() {
    std::lock_guard<std::mutex> t(mTimeMutex);
    syncBoundariesLocked();
    if (!mCurInfo.sampleRate) return 0;
    return (int64_t)(mCurInfo.totalFrames * 1000 / mCurInfo.sampleRate);
}

int AudioEngine::transitions() {
    std::lock_guard<std::mutex> t(mTimeMutex);
    syncBoundariesLocked();
    return mTransitions;
}

StreamInfo AudioEngine::currentInfo() {
    std::lock_guard<std::mutex> t(mTimeMutex);
    return mCurInfo;
}

std::string AudioEngine::describe() {
    StreamInfo i = currentInfo();
    char buf[256];
    if (!mStream) {
        std::snprintf(buf, sizeof(buf), "Fuente: %u Hz / %u bit / %u ch (stream cerrado)", i.sampleRate, i.bitsPerSample, i.channels);
    } else {
        std::snprintf(buf, sizeof(buf), "Fuente %u Hz/%ubit -> Salida %d Hz %s | %s | %s | %s",
                      i.sampleRate, i.bitsPerSample, mStream->getSampleRate(),
                      oboe::convertToText(mStream->getFormat()),
                      oboe::convertToText(mStream->getSharingMode()),
                      oboe::convertToText(mStream->getPerformanceMode()),
                      oboe::convertToText(mStream->getAudioApi()));
    }
    return buf;
}
