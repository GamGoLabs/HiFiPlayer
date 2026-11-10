#pragma once
// Remuestreador polifasico de alta calidad: sinc con ventana Kaiser (~100 dB), tabla de fases + interpolacion lineal.
// Streaming: el resultado NO depende de como se trocee la entrada. Sin retardo de grupo en la salida:
// y[m] corresponde al instante m*inRate/outRate de la entrada (la "latencia" es solo de disponibilidad de datos).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

class Resampler {
public:
    static constexpr int kPhases = 512;

    Resampler(uint32_t inRate, uint32_t outRate, uint32_t channels, int halfTaps0 = 128, double attenDb = 100.0)
        : mCh(channels), mStep(static_cast<double>(inRate) / static_cast<double>(outRate)) {
        const double ratio = std::min(1.0, static_cast<double>(outRate) / static_cast<double>(inRate));
        mHalf = std::min(1024, std::max(8, static_cast<int>(std::ceil(halfTaps0 / ratio))));
        design(ratio, attenDb);
        reset();
    }

    void reset() {
        mBuf.assign(mCh, std::vector<float>(static_cast<size_t>(mHalf - 1), 0.0f));   // historial de silencio
        mIdx = static_cast<size_t>(mHalf - 1);                                         // indice de x[0]
        mFrac = 0.0;
    }

    uint32_t latencyFrames() const { return static_cast<uint32_t>(mHalf); }            // frames de ENTRADA
    size_t maxOutputFrames(size_t inFrames) const {
        return static_cast<size_t>(std::ceil((static_cast<double>(inFrames) + 2.0 * mHalf) / mStep)) + 2;
    }

    // in/out entrelazados. Devuelve frames producidos (<= outCapFrames).
    size_t process(const float* in, size_t inFrames, float* out, size_t outCapFrames) {
        for (uint32_t c = 0; c < mCh; ++c) {
            auto& b = mBuf[c];
            const size_t old = b.size();
            b.resize(old + inFrames);
            for (size_t i = 0; i < inFrames; ++i) b[old + i] = in[i * mCh + c];
        }
        const size_t avail = mBuf[0].size();
        const int T = 2 * mHalf;
        size_t produced = 0;
        while (produced < outCapFrames) {
            const size_t i = mIdx;
            if (i + static_cast<size_t>(mHalf) >= avail) break;                // faltan muestras futuras
            const double fp = mFrac * kPhases;
            int p = static_cast<int>(fp);
            float w = static_cast<float>(fp - p);
            if (p >= kPhases) { p = kPhases - 1; w = 1.0f; }                   // salvaguarda de redondeo
            const float* r0 = &mTable[static_cast<size_t>(p) * T];
            const float* r1 = r0 + T;
            const size_t first = i - static_cast<size_t>(mHalf - 1);
            for (uint32_t c = 0; c < mCh; ++c) {
                const float* x = &mBuf[c][first];
                float a0 = 0.f, a1 = 0.f;
                for (int k = 0; k < T; ++k) { a0 += x[k] * r0[k]; a1 += x[k] * r1[k]; }
                out[produced * mCh + c] = a0 + w * (a1 - a0);
            }
            ++produced;
            mFrac += mStep;                                  // indice entero y fraccion por separado:
            const size_t whole = static_cast<size_t>(mFrac); // el resultado no depende del troceado
            mIdx += whole;
            mFrac -= static_cast<double>(whole);
        }
        const size_t keep = static_cast<size_t>(mHalf - 1);
        const size_t drop = mIdx >= keep ? mIdx - keep : 0;
        if (drop > 0) {
            for (auto& b : mBuf) b.erase(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(drop));
            mIdx -= drop;
        }
        return produced;
    }

private:
    static double bessel0(double x) {
        double sum = 1.0, term = 1.0;
        const double q = x * x / 4.0;
        for (int k = 1; k < 60; ++k) { term *= q / (static_cast<double>(k) * k); sum += term; if (term < 1e-18 * sum) break; }
        return sum;
    }

    void design(double ratio, double A) {
        const int T = 2 * mHalf;
        const double beta = 0.1102 * (A - 8.7);
        const double df = ((A - 8.0) / (2.285 * (T - 1))) / (2.0 * M_PI);   // ancho de transicion (ciclos/muestra de entrada)
        const double fc = 0.5 * ratio - df / 2.0;                           // la banda eliminada empieza en el Nyquist de salida
        const double i0b = bessel0(beta);
        mTable.assign(static_cast<size_t>(kPhases + 1) * T, 0.0f);
        std::vector<double> tmp(static_cast<size_t>(T));
        for (int p = 0; p <= kPhases; ++p) {
            const double frac = static_cast<double>(p) / kPhases;
            double sum = 0.0;
            for (int k = 0; k < T; ++k) {
                const double t = static_cast<double>(k - (mHalf - 1)) - frac;
                const double x = t / mHalf;
                const double win = std::fabs(x) < 1.0 ? bessel0(beta * std::sqrt(1.0 - x * x)) / i0b : 0.0;
                const double h = std::fabs(t) < 1e-12 ? 2.0 * fc : std::sin(2.0 * M_PI * fc * t) / (M_PI * t);
                tmp[k] = h * win;
                sum += tmp[k];
            }
            for (int k = 0; k < T; ++k) mTable[static_cast<size_t>(p) * T + k] = static_cast<float>(tmp[k] / sum);   // ganancia DC = 1
        }
    }

    uint32_t mCh;
    double mStep;
    int mHalf = 128;
    size_t mIdx = 0;
    double mFrac = 0.0;
    std::vector<float> mTable;
    std::vector<std::vector<float>> mBuf;
};
