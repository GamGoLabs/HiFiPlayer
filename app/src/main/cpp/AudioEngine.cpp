#include "AudioEngine.h"
#include "Log.h"
#include "Resampler.h"
#include "SampleConvert.h"

#include <oboe/OboeExtensions.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace std::chrono_literals;

static oboe::AudioFormat toOboe(OutFmt f) {
    switch (f) {
        case OutFmt::I16: return oboe::AudioFormat::I16;
        case OutFmt::I24: return oboe::AudioFormat::I24;
        case OutFmt::I32: return oboe::AudioFormat::I32;
        default: return oboe::AudioFormat::Float;
    }
}

AudioEngine::AudioEngine() { LOGD("AudioEngine creado"); }
AudioEngine::~AudioEngine() {
    std::lock_guard<std::mutex> g(mMutex);
    stopRecoveryLocked();
    stopLocked();
}

bool AudioEngine::compatible(const StreamInfo& a, const StreamInfo& b) const {
    if (a.sampleRate != b.sampleRate || a.channels != b.channels) return false;
    // con bit-perfect activo, el formato de salida abierto debe seguir siendo sin perdida para la pista siguiente
    if (mBitPerfect.load() && mStream && !formatLossless(mOutFmt, static_cast<int>(b.bitsPerSample))) return false;
    return true;
}

void AudioEngine::appendTel(const char* fmt, ...) {
    char buf[192];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> t(mTelMutex);
    mTelemetry += buf;
}

// ---------------------------------------------------------------- control

void AudioEngine::setOutputPlan(int deviceId, const std::vector<int>& rates, const std::vector<int>& formats,
                                int forcedRate, int forcedFmt) {
    std::lock_guard<std::mutex> g(mMutex);
    OutCaps caps;
    caps.rates = rates;
    for (int f : formats) if (f >= 0 && f <= 3) caps.formats.push_back(static_cast<OutFmt>(f));
    if (deviceId != mDeviceId || !(caps == mCaps)) mNegCache.clear();    // otro DAC: renegociar
    mDeviceId = deviceId;
    mCaps = caps;
    mForcedRate = forcedRate;
    mForcedFmt = forcedFmt;
    LOGD("plan de salida: dev=%d, %zu frecuencias, %zu formatos, mezclador forzado=%d/%d",
         deviceId, rates.size(), caps.formats.size(), forcedRate, forcedFmt);
}

bool AudioEngine::load(int fd) {
    std::lock_guard<std::mutex> g(mMutex);
    stopRecoveryLocked();
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
    mSuspended = false;
    mState = Ready;
    LOGD("load OK (bitPerfect=%d)", static_cast<int>(mBitPerfect.load()));
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

bool AudioEngine::tryOpen(int rate, OutFmt fmt, bool exclusive, int deviceId,
                          std::shared_ptr<oboe::AudioStream>& out, oboe::Result& r) {
    oboe::AudioStreamBuilder b;
    b.setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)       // habilita MMAP/exclusivo si el HAL lo permite
            ->setSharingMode(exclusive ? oboe::SharingMode::Exclusive : oboe::SharingMode::Shared)
            ->setUsage(oboe::Usage::Media)
            ->setContentType(oboe::ContentType::Music)
            ->setFormat(toOboe(fmt))
            ->setChannelCount(static_cast<int32_t>(mChannels))
            ->setSampleRate(rate)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::None)
            ->setFormatConversionAllowed(false)
            ->setChannelConversionAllowed(false)
            ->setDataCallback(this)
            ->setErrorCallback(this);
    if (deviceId > 0) b.setDeviceId(deviceId);                         // ruteo explicito al DAC
    r = b.openStream(out);
    if (r != oboe::Result::OK) { out.reset(); return false; }
    return true;
}

// Negociacion en cascada. AAudio concede EXCLUSIVO solo si el HAL/DAC lo permite para esa combinacion
// frecuencia+formato; si no, devuelve un stream COMPARTIDO sin avisar. Por eso se comprueba el modo REAL
// concedido y se prueba la siguiente combinacion. El resultado se cachea por (dispositivo, frecuencia, profundidad).
bool AudioEngine::openStreamLocked() {
    if (mStream) return true;
    const StreamInfo in = currentInfo();
    mChannels = in.channels;
    const bool bp = mBitPerfect.load();
    const int srcRate = static_cast<int>(in.sampleRate);

    std::string trace;
    std::shared_ptr<oboe::AudioStream> got;
    OutFmt gotFmt = OutFmt::Float;
    bool gotForced = false;
    int attempts = 0;

    auto addTrace = [&](const char* line) {
        if (!trace.empty()) trace += " | ";
        trace += line;
        LOGD("NEG %s", line);
    };

    auto attempt = [&](const Candidate& c, bool acceptShared) -> bool {
        ++attempts;
        std::shared_ptr<oboe::AudioStream> s;
        oboe::Result r = oboe::Result::OK;
        char line[200];
        if (!tryOpen(c.rate, c.fmt, true, mDeviceId, s, r)) {
            std::snprintf(line, sizeof(line), "%dHz/%s: error %s", c.rate, fmtName(c.fmt), oboe::convertToText(r));
            addTrace(line);
            return false;
        }
        const bool excl = s->getSharingMode() == oboe::SharingMode::Exclusive;
        const bool mmap = oboe::OboeExtensions::isMMapUsed(s.get());
        std::snprintf(line, sizeof(line), "%dHz/%s: %s%s%s", c.rate, fmtName(c.fmt),
                      excl ? "EXCLUSIVO" : "compartido", mmap ? "+MMAP" : "",
                      (!excl && acceptShared) ? " (aceptado: mezclador bit-perfect)" : "");
        addTrace(line);
        if (excl || acceptShared) { got = s; gotFmt = c.fmt; return true; }
        s->close();
        return false;
    };

    if (bp) {
        char kb[96];
        std::snprintf(kb, sizeof(kb), "%d|%d|%u|%u", mDeviceId, srcRate, in.bitsPerSample, in.channels);
        const std::string key = kb;
        auto it = mNegCache.find(key);
        const bool skipExcl = (it != mNegCache.end() && it->second.noExclusive);

        std::vector<Candidate> plan;
        const bool hasForced = mForcedRate > 0 && mForcedFmt >= 0 && mForcedFmt <= 3;
        if (hasForced) plan.push_back({mForcedRate, static_cast<OutFmt>(mForcedFmt), mForcedRate != srcRate});

        std::vector<Candidate> base;
        if (skipExcl) addTrace("cache: este DAC no concede exclusivo con ninguna combinacion probada");
        else base = buildPlan(srcRate, static_cast<int>(in.bitsPerSample), mCaps);
        if (it != mNegCache.end() && !it->second.noExclusive) {          // probar primero lo que funciono antes
            for (size_t i = 0; i < base.size(); ++i)
                if (base[i].rate == it->second.rate && static_cast<int>(base[i].fmt) == it->second.fmt) {
                    std::swap(base[0], base[i]);
                    break;
                }
        }
        plan.insert(plan.end(), base.begin(), base.end());

        for (size_t i = 0; i < plan.size() && !got; ++i) {
            const bool forced = hasForced && i == 0;
            if (attempt(plan[i], forced)) gotForced = forced;
        }
        if (got && !gotForced) mNegCache[key] = {static_cast<int>(got->getSampleRate()), static_cast<int>(gotFmt), false};
        else if (!got && !skipExcl && !base.empty()) mNegCache[key] = {0, 0, true};
    }

    if (!got) {   // ultimo recurso: compartido a la frecuencia de la pista (comportamiento anterior)
        OutFmt nf = bp ? (in.bitsPerSample <= 16 ? OutFmt::I16 : OutFmt::I32) : OutFmt::Float;
        std::shared_ptr<oboe::AudioStream> s;
        oboe::Result r = oboe::Result::OK;
        bool ok = tryOpen(srcRate, nf, false, mDeviceId, s, r);
        if (!ok && mDeviceId > 0) { addTrace("compartido con dispositivo explicito fallo: sin deviceId"); ok = tryOpen(srcRate, nf, false, 0, s, r); }
        if (!ok && nf != OutFmt::Float) {
            LOGW("Formato nativo %s rechazado (%s). Fallback a Float (NO bit-perfect)", fmtName(nf), oboe::convertToText(r));
            nf = OutFmt::Float;
            ok = tryOpen(srcRate, nf, false, 0, s, r);
        }
        if (!ok) { LOGE("openStream fallo: %s", oboe::convertToText(r)); return false; }
        got = s;
        gotFmt = nf;
        addTrace("fallback: compartido");
    }

    mStream = got;
    mOutFmt = gotFmt;
    mFrameBytes = mChannels * static_cast<uint32_t>(sc::bytesPerSample(gotFmt));
    mStreamRate = static_cast<uint32_t>(mStream->getSampleRate());

    // Verificación de buffer size real
    const int burst = mStream->getFramesPerBurst();
    const int requestedBuffer = burst * 8;
    const auto bufferResult = mStream->setBufferSizeInFrames(requestedBuffer);
    const int actualBuffer = bufferResult.value();

    if (actualBuffer != requestedBuffer) {
        LOGW("Buffer size ajustado: solicitado=%d, real=%d (burst=%d)", requestedBuffer, actualBuffer, burst);
        if (actualBuffer % burst != 0) {
            LOGE("Buffer size no es múltiplo de burst! Puede causar glitches en modo exclusivo");
        }
    }
    LOGD("Buffer configurado: %d frames (%.1f ms), burst=%d frames", actualBuffer,
         actualBuffer * 1000.0 / mStreamRate.load(), burst);

    mRing = std::make_unique<RingBuffer>(static_cast<size_t>(mStreamRate.load()) * mFrameBytes);   // ~1 s
    mGainStep = 1.0f / (0.08f * static_cast<float>(mStreamRate.load()));
    mCurGain = mTargetGain.load();

    const bool excl = mStream->getSharingMode() == oboe::SharingMode::Exclusive;
    const bool mmap = oboe::OboeExtensions::isMMapUsed(mStream.get());
    const bool resampled = mStreamRate.load() != in.sampleRate;
    mResamplerActive.store(resampled);

    const char* why = !bp ? "bit-perfect desactivado"
                          : (mDeviceId == 0 ? "sin DAC USB (salida estandar)"
                                            : ((!excl && !gotForced) ? "exclusivo rechazado por el sistema" : ""));
    const char* mode = gotForced ? "MIXER_BP" : (excl ? "EXCL" : "SHARED");

    char tel[1200];
    std::snprintf(tel, sizeof(tel),
                  "mode=%s;excl=%d;mmap=%d;api=%s;dev=%d;rate=%u;fmt=%s;src=%u;bits=%u;resampled=%d;attempts=%d;why=%s;buffer=%d;neg=%s",
                  mode, excl ? 1 : 0, mmap ? 1 : 0, oboe::convertToText(mStream->getAudioApi()), mStream->getDeviceId(),
                  mStreamRate.load(), fmtName(gotFmt), in.sampleRate, in.bitsPerSample, resampled ? 1 : 0, attempts, why,
                  actualBuffer, trace.c_str());

    if (bp && !excl && !gotForced && mDeviceId > 0) {
        std::string warning = "WARNING: exclusive mode unavailable; using shared mode (NOT bit-perfect)";
        std::snprintf(tel + std::strlen(tel), sizeof(tel) - std::strlen(tel), ";%s", warning.c_str());
        LOGW("%s", warning.c_str());
    }

    { std::lock_guard<std::mutex> t(mTelMutex); mTelemetry = tel; }
    LOGD("Stream final: %s", tel);
    if (resampled)
        LOGW("Resampling %u -> %u Hz en C++ (sinc Kaiser ~100 dB): salida NO bit-exacta", in.sampleRate, mStreamRate.load());
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
    if (s == Suspended) {                       // recuperacion inmediata pedida por el usuario
        stopRecoveryLocked();
        if (recoverNowLocked()) return true;
        mState = Error;
        return false;
    }
    if (s == Paused) {
        stopRecoveryLocked();
        if (mStream) { mStream->requestStart(); mState = Playing; return true; }
        if (mSuspended) {                       // pausa tras desconexion: reabre desde la posicion guardada
            if (recoverNowLocked()) return true;
            mState = Error;
            return false;
        }
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
    stopRecoveryLocked();
    if (mState == Suspended) {                  // cancelar recovery: queda en pausa con posicion conservada
        mSuspended = false;
        mState = Paused;
        appendTel(";recover=cancel");
        LOGD("Recovery cancelado por pausa; posicion conservada");
        return;
    }
    if (mState == Playing && mStream) { mStream->requestPause(); mState = Paused; }
}

void AudioEngine::stop() {
    std::lock_guard<std::mutex> g(mMutex);
    if (!mSource) return;
    stopRecoveryLocked();
    stopLocked();
    { std::lock_guard<std::mutex> s(mSrcMutex); mNext.reset(); }
    mSource->seek(0);
    mStartFrame = 0;
    mSuspended = false;
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

    if (s == Suspended) {                       // redefine el punto de reanudacion
        stopRecoveryLocked();
        StreamInfo info;
        { std::lock_guard<std::mutex> t(mTimeMutex); info = mCurInfo; }
        uint64_t target = static_cast<uint64_t>(ms < 0 ? 0 : ms) * info.sampleRate / 1000;
        if (info.totalFrames && target >= info.totalFrames) target = info.totalFrames - 1;
        if (!mSource->seek(target)) { LOGE("seek fallo"); return false; }
        mResumeFrame = target; mStartFrame = target;
        { std::lock_guard<std::mutex> t(mTimeMutex); mStartOffset = target; }
        LOGD("seek en suspension -> %lld ms", static_cast<long long>(ms));
        return true;
    }

    bool resume = (s == Playing);
    bool streaming = (s == Playing || s == Paused || s == Ended);
    if (streaming) { stopDecoderLocked(); if (mStream) mStream->stop(); }

    StreamInfo info;
    {   // Limitacion conocida: si el decoder ya paso a la pista siguiente, el seek aplica a esa
        std::lock_guard<std::mutex> t(mTimeMutex);
        if (!mBoundaries.empty()) {
            LOGW("Seek con transicion gapless pendiente: se aplica a la pista siguiente");
            mTransitions += static_cast<int>(mBoundaries.size());
            mCurInfo = mBoundaries.back().info;
            mBoundaries.clear();
        }
        info = mCurInfo;
    }
    uint64_t target = static_cast<uint64_t>(ms < 0 ? 0 : ms) * info.sampleRate / 1000;
    if (info.totalFrames && target >= info.totalFrames) target = info.totalFrames - 1;
    if (!mSource->seek(target)) { LOGE("seek fallo"); return false; }
    LOGD("seek -> %lld ms (frame %llu)", static_cast<long long>(ms), static_cast<unsigned long long>(target));

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

// ---------------------------------------------------------------- hot-plug / auto-recuperacion

void AudioEngine::handleDisconnectLocked() {
    stopDecoderLocked();
    stopRecoveryLocked();
    {
        std::lock_guard<std::mutex> t(mTimeMutex);
        const uint64_t outRate = mStreamRate.load() ? mStreamRate.load() : mCurInfo.sampleRate;
        const uint64_t rendered = mFramesRendered.load() > mOriginRendered ? mFramesRendered.load() - mOriginRendered : 0;
        const uint64_t srcFrames = outRate ? rendered * mCurInfo.sampleRate / outRate : 0;   // compensa resampler
        mResumeFrame = mStartOffset + srcFrames;
        if (mCurInfo.totalFrames && mResumeFrame >= mCurInfo.totalFrames) mResumeFrame = mCurInfo.totalFrames - 1;
        mStartOffset = mResumeFrame;          // positionMs() queda estable en el punto de reanudacion
        mOriginRendered = mFramesRendered.load();
        mBoundaries.clear();                  // referian a frames de SALIDA del stream muerto
    }
    mWasPlaying = (mState == Playing);
    mStream.reset();                          // Oboe ya cerro el stream tras el error
    mRing.reset();
    mSuspended = true;
    mState = Suspended;
    appendTel(";suspend=1;resumeFrame=%llu", static_cast<unsigned long long>(mResumeFrame));
    LOGW("SUSPEND: DAC desconectado; reanudacion preparada en frame %llu (%s)",
         static_cast<unsigned long long>(mResumeFrame), mWasPlaying ? "reproducia" : "en pausa");
}

bool AudioEngine::recoverNowLocked() {
    if (mState != Suspended) return true;
    if (!mSource) return false;
    if (!mSource->seek(mResumeFrame)) { LOGE("recover: seek a frame %llu fallo", static_cast<unsigned long long>(mResumeFrame)); return false; }
    mStartFrame = mResumeFrame;
    if (!openStreamLocked()) return false;    // usa el plan vigente (Kotlin pudo actualizarlo al reconectar)
    resetTimelineLocked(mResumeFrame);
    startDecoderLocked();
    prefill();
    auto r = mStream->requestStart();
    if (r != oboe::Result::OK) { LOGE("recover: requestStart: %s", oboe::convertToText(r)); stopLocked(); return false; }
    mSuspended = false;
    mState = Playing;
    return true;
}

void AudioEngine::startRecoveryLocked() {
    stopRecoveryLocked();
    mRecoverRun = true;
    mRecoverAttempts = 0;
    mRecoverThread = std::thread(&AudioEngine::recoveryLoop, this);
}

void AudioEngine::stopRecoveryLocked() {
    mRecoverRun = false;
    if (mRecoverThread.joinable()) mRecoverThread.join();
}

// Hilo de recuperacion: NUNCA bloquea en mMutex (try_lock), para que stopRecoveryLocked()
// (que hace join bajo mMutex) no pueda deadlockear contra su propio hilo.
void AudioEngine::recoveryLoop() {
    constexpr auto kTimeout = 20s;
    const auto t0 = std::chrono::steady_clock::now();
    while (mRecoverRun.load()) {
        std::this_thread::sleep_for(800ms);
        if (!mRecoverRun.load()) break;
        ++mRecoverAttempts;
        if (mMutex.try_lock()) {
            if (mState != Suspended) { mMutex.unlock(); break; }
            const bool ok = recoverNowLocked();
            mMutex.unlock();
            if (ok) {
                appendTel(";recover=ok;attempts=%d", mRecoverAttempts);
                LOGD("RECOVER OK: stream reabierto y reanudado tras %d intento(s)", mRecoverAttempts);
                break;
            }
            if (std::chrono::steady_clock::now() - t0 > kTimeout) {
                appendTel(";recover=timeout;attempts=%d", mRecoverAttempts);
                LOGW("RECOVER timeout tras %d intento(s): queda en PAUSA con posicion conservada", mRecoverAttempts);
                if (mMutex.try_lock()) { mSuspended = false; mState = Paused; mMutex.unlock(); }
                break;
            }
        }
    }
}

// ---------------------------------------------------------------- decoder thread

void AudioEngine::decoderLoop() {
    constexpr size_t kChunk = 4096;
    const uint32_t ch = mChannels;
    const uint32_t streamRate = mStreamRate.load();
    const OutFmt fmt = mOutFmt;
    const uint32_t frameBytes = mFrameBytes;

    std::unique_ptr<Resampler> rs;
    if (mSource->info().sampleRate != streamRate) {
        rs = std::make_unique<Resampler>(mSource->info().sampleRate, streamRate, ch);
        LOGD("decoder: resampler %u -> %u Hz (%u canales)", mSource->info().sampleRate, streamRate, ch);
    }
    const size_t maxOut = rs ? rs->maxOutputFrames(kChunk) : kChunk;
    std::vector<int32_t> pcm(kChunk * ch);
    std::vector<float> fin, fout;
    if (rs) { fin.resize(kChunk * ch); fout.resize(maxOut * ch); }
    std::vector<uint8_t> out(maxOut * frameBytes);

    uint64_t lastUnderruns = 0;
    uint64_t srcPos = mStartFrame;   // frame alcanzado dentro del archivo actual (origen)
    LOGD("decoder: inicio (desde frame %llu)", static_cast<unsigned long long>(srcPos));

    auto writeOut = [&](size_t frames) {   // espera hueco y escribe
        const size_t bytes = frames * frameBytes;
        while (mRun.load() && mRing->writable() < bytes) std::this_thread::sleep_for(5ms);
        if (mRun.load()) { mRing->write(out.data(), bytes); mFramesWritten += frames; }
    };

    while (mRun.load()) {
        uint64_t u = mUnderruns.load();
        if (u != lastUnderruns) { LOGW("underruns acumulados: %llu", static_cast<unsigned long long>(u)); lastUnderruns = u; }

        if (mRing->writable() < out.size()) { std::this_thread::sleep_for(5ms); continue; }

        uint64_t n = mSource->readFrames(pcm.data(), kChunk);
        if (n == 0) {
            const uint64_t declared = mSource->info().totalFrames;
            if (declared && srcPos != declared)
                LOGW("EOF en frame %llu pero la cabecera declara %llu (dif %lld frames = %.1f s): "
                     "archivo truncado, cabecera erronea o error de decodificacion",
                     static_cast<unsigned long long>(srcPos), static_cast<unsigned long long>(declared),
                     static_cast<long long>(declared) - static_cast<long long>(srcPos),
                     static_cast<double>(static_cast<long long>(declared) - static_cast<long long>(srcPos)) / mSource->info().sampleRate);
            else
                LOGD("EOF correcto en frame %llu", static_cast<unsigned long long>(srcPos));
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
                mBoundaries.push_back({mFramesWritten, mSource->info()});   // el resampler sigue: sin hueco
                LOGD("decoder: gapless en frame de salida %llu", static_cast<unsigned long long>(mFramesWritten));
                continue;
            }
            if (rs) {   // fin real de la cola: vaciar la cola del filtro con silencio
                std::vector<float> zeros(static_cast<size_t>(rs->latencyFrames()) * ch, 0.0f);
                size_t frames = rs->process(zeros.data(), rs->latencyFrames(), fout.data(), maxOut);
                if (frames) { sc::fromFloat(fout.data(), frames * ch, fmt, out.data()); writeOut(frames); }
            }
            LOGD("decoder: EOF");
            mEof.store(true, std::memory_order_release);
            break;
        }
        size_t frames;
        if (!rs) {
            sc::fromInt32(pcm.data(), n * ch, fmt, out.data());
            frames = static_cast<size_t>(n);
        } else {
            for (size_t i = 0; i < n * ch; ++i) fin[i] = static_cast<float>(pcm[i]) * (1.0f / 2147483648.0f);
            frames = rs->process(fin.data(), static_cast<size_t>(n), fout.data(), maxOut);
            sc::fromFloat(fout.data(), frames * ch, fmt, out.data());
        }
        if (frames) writeOut(frames);
        srcPos += n;
    }
    LOGD("decoder: fin");
}

// ---------------------------------------------------------------- callback (tiempo real: sin logs, sin locks, sin malloc)

oboe::DataCallbackResult AudioEngine::onAudioReady(oboe::AudioStream*, void* data, int32_t numFrames) {
    const size_t want = static_cast<size_t>(numFrames) * mFrameBytes;
    auto* dst = static_cast<uint8_t*>(data);
    size_t got = mRing->read(dst, want);
    mFramesRendered.fetch_add(got / mFrameBytes, std::memory_order_relaxed);
    if (got) sc::applyGain(mOutFmt, data, got / mFrameBytes, mChannels, mCurGain,
                           mTargetGain.load(std::memory_order_relaxed), mGainStep);
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
    if (r == oboe::Result::ErrorDisconnected) {
        std::lock_guard<std::mutex> g(mMutex);
        if (mState == Suspended) return;                 // ya gestionado
        if (mState == Playing || mState == Paused) {
            handleDisconnectLocked();
            if (mWasPlaying) startRecoveryLocked();      // en pausa no hay prisa: play() recuperara
            return;
        }
    }
    mState = Error;   // Kotlin decide si reintenta
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
    const uint64_t outRate = mStreamRate.load() ? mStreamRate.load() : mCurInfo.sampleRate;
    const int64_t startMs = static_cast<int64_t>(mStartOffset * 1000 / mCurInfo.sampleRate);          // origen
    const int64_t playedMs = static_cast<int64_t>((mFramesRendered.load() - mOriginRendered) * 1000 / outRate);   // salida
    return startMs + playedMs;
}

int64_t AudioEngine::durationMs() {
    std::lock_guard<std::mutex> t(mTimeMutex);
    syncBoundariesLocked();
    if (!mCurInfo.sampleRate) return 0;
    return static_cast<int64_t>(mCurInfo.totalFrames * 1000 / mCurInfo.sampleRate);
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

std::string AudioEngine::outputInfo() {
    std::lock_guard<std::mutex> t(mTelMutex);
    return mTelemetry;
}

std::string AudioEngine::describe() {
    StreamInfo i = currentInfo();
    char buf[320];
    if (!mStream) {
        std::snprintf(buf, sizeof(buf), "Fuente: %u Hz / %u bit / %u ch (stream cerrado)", i.sampleRate, i.bitsPerSample, i.channels);
    } else {
        const bool resampled = static_cast<uint32_t>(mStream->getSampleRate()) != i.sampleRate;
        std::snprintf(buf, sizeof(buf), "Fuente %u Hz/%ubit -> Salida %d Hz %s%s | %s | %s | %s | dev=%d%s",
                      i.sampleRate, i.bitsPerSample, mStream->getSampleRate(), fmtName(mOutFmt),
                      resampled ? " (resampleado)" : "",
                      oboe::convertToText(mStream->getSharingMode()),
                      oboe::convertToText(mStream->getPerformanceMode()),
                      oboe::convertToText(mStream->getAudioApi()), mStream->getDeviceId(),
                      oboe::OboeExtensions::isMMapUsed(mStream.get()) ? " | MMAP" : "");
    }
    return buf;
}