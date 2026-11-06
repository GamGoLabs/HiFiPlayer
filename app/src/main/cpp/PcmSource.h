#pragma once
#include <cstdint>
#include <memory>
#include <string>

struct StreamInfo {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t bitsPerSample = 0;
    uint64_t totalFrames = 0;
};

// Fuente PCM lossless. Siempre entrega int32 justificado a la izquierda
// (un FLAC de 16 bit llega como v<<16), lo que permite bajar a I16/I32 sin perder bits.
class PcmSource {
public:
    virtual ~PcmSource() = default;
    virtual const StreamInfo& info() const = 0;
    virtual uint64_t readFrames(int32_t* out, uint64_t frames) = 0;   // 0 = EOF
    virtual bool seek(uint64_t frame) = 0;

    // Abre desde un file descriptor (compatible con SAF / content://). No cierra el fd.
    static std::unique_ptr<PcmSource> openFd(int fd, std::string& error);
};
