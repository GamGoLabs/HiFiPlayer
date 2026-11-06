#include "PcmSource.h"
#include "Log.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

namespace {

// Lector sobre un fd propio (dup del que entrega Kotlin). No depende de /proc ni de permisos de ruta.
struct FdIO {
    int fd = -1;
    explicit FdIO(int src) : fd(dup(src)) {}
    ~FdIO() { if (fd >= 0) close(fd); }

    static size_t onRead(void* u, void* out, size_t n) {
        auto* io = static_cast<FdIO*>(u);
        size_t total = 0;
        auto* p = static_cast<uint8_t*>(out);
        while (total < n) {
            ssize_t r = read(io->fd, p + total, n - total);
            if (r < 0) { if (errno == EINTR) continue; break; }
            if (r == 0) break;
            total += static_cast<size_t>(r);
        }
        return total;
    }
    static bool seekTo(void* u, int offset, int whence) {
        return lseek(static_cast<FdIO*>(u)->fd, offset, whence) >= 0;
    }
    static bool tell(void* u, int64_t* cur) {
        off_t p = lseek(static_cast<FdIO*>(u)->fd, 0, SEEK_CUR);
        if (p < 0) return false;
        *cur = p;
        return true;
    }
};

class FlacSource final : public PcmSource {
public:
    FlacSource(std::unique_ptr<FdIO> io, drflac* f) : mIO(std::move(io)), mF(f) {
        mInfo = {f->sampleRate, f->channels, f->bitsPerSample, f->totalPCMFrameCount};
    }
    ~FlacSource() override { drflac_close(mF); }
    const StreamInfo& info() const override { return mInfo; }
    uint64_t readFrames(int32_t* out, uint64_t n) override { return drflac_read_pcm_frames_s32(mF, n, out); }
    bool seek(uint64_t f) override { return drflac_seek_to_pcm_frame(mF, f) == DRFLAC_TRUE; }
private:
    std::unique_ptr<FdIO> mIO;
    drflac* mF;
    StreamInfo mInfo;
};

class WavSource final : public PcmSource {
public:
    explicit WavSource(std::unique_ptr<FdIO> io) : mIO(std::move(io)) {}
    ~WavSource() override { if (mOk) drwav_uninit(&mW); }
    bool open() {
        auto seekCb = [](void* u, int off, drwav_seek_origin o) -> drwav_bool32 {
            return FdIO::seekTo(u, off, o == DRWAV_SEEK_SET ? SEEK_SET : (o == DRWAV_SEEK_CUR ? SEEK_CUR : SEEK_END));
        };
        auto tellCb = [](void* u, drwav_int64* c) -> drwav_bool32 {
            int64_t v; if (!FdIO::tell(u, &v)) return DRWAV_FALSE; *c = v; return DRWAV_TRUE;
        };
        mOk = drwav_init(&mW, FdIO::onRead, seekCb, tellCb, mIO.get(), nullptr) == DRWAV_TRUE;
        if (mOk) mInfo = {mW.sampleRate, mW.channels, mW.bitsPerSample, mW.totalPCMFrameCount};
        return mOk;
    }
    const StreamInfo& info() const override { return mInfo; }
    uint64_t readFrames(int32_t* out, uint64_t n) override { return drwav_read_pcm_frames_s32(&mW, n, out); }
    bool seek(uint64_t f) override { return drwav_seek_to_pcm_frame(&mW, f) == DRWAV_TRUE; }
private:
    std::unique_ptr<FdIO> mIO;
    drwav mW{};
    bool mOk = false;
    StreamInfo mInfo;
};

}  // namespace

std::unique_ptr<PcmSource> PcmSource::openFd(int fd, std::string& error) {
    auto io = std::make_unique<FdIO>(fd);
    if (io->fd < 0) { error = "dup(fd) fallo"; LOGE("%s errno=%d", error.c_str(), errno); return nullptr; }

    uint8_t m[4] = {0};
    if (pread(io->fd, m, 4, 0) != 4) {
        error = "No se pudo leer la cabecera";
        LOGE("%s (errno=%d)", error.c_str(), errno);
        return nullptr;
    }
    lseek(io->fd, 0, SEEK_SET);

    if (!std::memcmp(m, "fLaC", 4) || !std::memcmp(m, "ID3", 3)) {
        auto seekCb = [](void* u, int off, drflac_seek_origin o) -> drflac_bool32 {
            return FdIO::seekTo(u, off, o == DRFLAC_SEEK_SET ? SEEK_SET : (o == DRFLAC_SEEK_CUR ? SEEK_CUR : SEEK_END));
        };
        auto tellCb = [](void* u, drflac_int64* c) -> drflac_bool32 {
            int64_t v; if (!FdIO::tell(u, &v)) return DRFLAC_FALSE; *c = v; return DRFLAC_TRUE;
        };
        FdIO* raw = io.get();
        drflac* f = drflac_open(FdIO::onRead, seekCb, tellCb, raw, nullptr);
        if (!f) { error = "FLAC invalido o no soportado"; LOGE("%s", error.c_str()); return nullptr; }
        LOGD("FLAC abierto: %u Hz, %u ch, %u bit, %llu frames", f->sampleRate, f->channels,
             f->bitsPerSample, (unsigned long long)f->totalPCMFrameCount);
        return std::make_unique<FlacSource>(std::move(io), f);
    }
    if (!std::memcmp(m, "RIFF", 4) || !std::memcmp(m, "RF64", 4)) {
        auto w = std::make_unique<WavSource>(std::move(io));
        if (!w->open()) { error = "WAV invalido"; LOGE("%s", error.c_str()); return nullptr; }
        LOGD("WAV abierto: %u Hz, %u ch, %u bit", w->info().sampleRate, w->info().channels, w->info().bitsPerSample);
        return w;
    }
    error = "Formato no soportado en Fase 1";
    LOGE("%s (cabecera %02x %02x %02x %02x)", error.c_str(), m[0], m[1], m[2], m[3]);
    return nullptr;
}
