#pragma once
// Ring buffer SPSC lock-free (1 productor = hilo decoder, 1 consumidor = callback de audio).
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

class RingBuffer {
public:
    explicit RingBuffer(size_t capacityBytes) : mBuf(capacityBytes), mCap(capacityBytes) {}

    size_t capacity() const { return mCap; }
    size_t readable() const {                       // r antes que w => nunca negativo
        size_t r = mR.load(std::memory_order_acquire);
        size_t w = mW.load(std::memory_order_acquire);
        return w - r;
    }
    size_t writable() const { return mCap - readable(); }

    size_t write(const uint8_t* src, size_t n) {
        size_t w = mW.load(std::memory_order_relaxed);
        size_t r = mR.load(std::memory_order_acquire);
        n = std::min(n, mCap - (w - r));
        size_t idx = w % mCap, first = std::min(n, mCap - idx);
        std::memcpy(mBuf.data() + idx, src, first);
        std::memcpy(mBuf.data(), src + first, n - first);
        mW.store(w + n, std::memory_order_release);
        return n;
    }

    size_t read(uint8_t* dst, size_t n) {
        size_t r = mR.load(std::memory_order_relaxed);
        size_t w = mW.load(std::memory_order_acquire);
        n = std::min(n, w - r);
        size_t idx = r % mCap, first = std::min(n, mCap - idx);
        std::memcpy(dst, mBuf.data() + idx, first);
        std::memcpy(dst + first, mBuf.data(), n - first);
        mR.store(r + n, std::memory_order_release);
        return n;
    }

    // Solo con productor y consumidor detenidos.
    void reset() { mW.store(0); mR.store(0); }

private:
    std::vector<uint8_t> mBuf;
    size_t mCap;
    std::atomic<size_t> mW{0}, mR{0};
};
