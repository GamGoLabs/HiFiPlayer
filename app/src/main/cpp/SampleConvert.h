#pragma once
// Conversiones de formato y ganancia. Header-only para probar en el PC el MISMO codigo que corre en el movil.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>

#include "OutputPlanner.h"

namespace sc {

    inline size_t bytesPerSample(OutFmt f) { return f == OutFmt::I16 ? 2 : (f == OutFmt::I24 ? 3 : 4); }

    inline void put24(uint8_t* p, int32_t v) {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    }
    inline int32_t get24(const uint8_t* p) {
        return static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) | (static_cast<uint32_t>(p[1]) << 16) |
                                    (static_cast<uint32_t>(p[2]) << 24)) >> 8;     // extiende signo
    }

// int32 justificado a la izquierda (salida del decodificador) -> formato de salida. Sin perdida si el formato alcanza.
// DITHERING TPDF aplicado al convertir a Float para enmascarar ruido de cuantizacion (estandar audiófilo).
    inline void fromInt32(const int32_t* in, size_t n, OutFmt f, uint8_t* out) {
        switch (f) {
            case OutFmt::I16: {
                auto* o = reinterpret_cast<int16_t*>(out);
                for (size_t i = 0; i < n; ++i) o[i] = static_cast<int16_t>(in[i] >> 16);
                break;
            }
            case OutFmt::I24: {
                for (size_t i = 0; i < n; ++i) put24(out + 3 * i, in[i] >> 8);
                break;
            }
            case OutFmt::I32:
                std::memcpy(out, in, n * sizeof(int32_t));
                break;
            default: {
                // Conversión int32 -> float con DITHERING TPDF (Triangular Probability Density Function)
                // TPDF = suma de dos distribuciones uniformes independientes en [-0.5 LSB, +0.5 LSB]
                // Enmascara el ruido de cuantización cuando se pierde precisión (32 bits -> 24 bits mantisa)
                auto* o = reinterpret_cast<float*>(out);
                thread_local std::mt19937 rng(std::random_device{}());
                std::uniform_real_distribution<float> dist(-0.5f, 0.5f);

                for (size_t i = 0; i < n; ++i) {
                    // Dithering TPDF: dos números aleatorios uniformes independientes
                    float dither = dist(rng) + dist(rng);
                    // Aplicar dither antes de la conversión para enmascarar ruido de cuantización
                    o[i] = static_cast<float>(static_cast<double>(in[i]) + dither) * (1.0f / 2147483648.0f);
                }
            }
        }
    }

    inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// float (+-1.0 = fondo de escala) -> formato de salida, con redondeo y saturacion.
    inline void fromFloat(const float* in, size_t n, OutFmt f, uint8_t* out) {
        switch (f) {
            case OutFmt::I16: {
                auto* o = reinterpret_cast<int16_t*>(out);
                for (size_t i = 0; i < n; ++i) o[i] = static_cast<int16_t>(std::lrintf(clampf(in[i] * 32768.0f, -32768.0f, 32767.0f)));
                break;
            }
            case OutFmt::I24: {
                for (size_t i = 0; i < n; ++i) put24(out + 3 * i, static_cast<int32_t>(std::lrintf(clampf(in[i] * 8388608.0f, -8388608.0f, 8388607.0f))));
                break;
            }
            case OutFmt::I32: {
                auto* o = reinterpret_cast<int32_t*>(out);
                for (size_t i = 0; i < n; ++i) {
                    double d = static_cast<double>(in[i]) * 2147483648.0;
                    d = d < -2147483648.0 ? -2147483648.0 : (d > 2147483647.0 ? 2147483647.0 : d);
                    o[i] = static_cast<int32_t>(std::llrint(d));
                }
                break;
            }
            default:
                std::memcpy(out, in, n * sizeof(float));
        }
    }

// Atenuacion con rampa. A volumen pleno y objetivo 1.0 NO toca ni un bit (camino bit-perfect).
    inline void applyGain(OutFmt f, void* data, size_t frames, uint32_t ch, float& cur, float target, float step) {
        float g = cur;
        if (g == target && target == 1.0f) return;
        auto next = [&]() {
            if (g < target) { g += step; if (g > target) g = target; }
            else if (g > target) { g -= step; if (g < target) g = target; }
        };
        switch (f) {
            case OutFmt::I16: {
                auto* p = static_cast<int16_t*>(data);
                for (size_t i = 0; i < frames; ++i) {
                    next();
                    for (uint32_t c = 0; c < ch; ++c, ++p)
                        *p = static_cast<int16_t>(static_cast<float>(*p) * g);
                }
                break;
            }
            case OutFmt::I24: {
                auto* p = static_cast<uint8_t*>(data);
                for (size_t i = 0; i < frames; ++i) {
                    next();
                    for (uint32_t c = 0; c < ch; ++c, p += 3)
                        put24(p, static_cast<int32_t>(static_cast<double>(get24(p)) * g));
                }
                break;
            }
            case OutFmt::I32: {
                auto* p = static_cast<int32_t*>(data);
                for (size_t i = 0; i < frames; ++i) {
                    next();
                    for (uint32_t c = 0; c < ch; ++c, ++p)
                        *p = static_cast<int32_t>(static_cast<double>(*p) * g);
                }
                break;
            }
            default: {
                auto* p = static_cast<float*>(data);
                for (size_t i = 0; i < frames; ++i) {
                    next();
                    for (uint32_t c = 0; c < ch; ++c, ++p)
                        *p *= g;
                }
            }
        }
        cur = g;
    }

}  // namespace sc