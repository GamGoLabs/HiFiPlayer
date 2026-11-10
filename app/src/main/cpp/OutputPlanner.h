#pragma once
// Planificador de negociacion de salida. Logica pura (sin Oboe) para poder probarla en el PC.
#include <algorithm>
#include <functional>
#include <vector>

enum class OutFmt : int { I16 = 0, I24 = 1, I32 = 2, Float = 3 };

inline const char* fmtName(OutFmt f) {
    switch (f) {
        case OutFmt::I16: return "PCM_16";
        case OutFmt::I24: return "PCM_24";
        case OutFmt::I32: return "PCM_32";
        default: return "PCM_FLOAT";
    }
}

// Un formato es "sin perdida" si representa exactamente todas las muestras de origen.
inline bool formatLossless(OutFmt f, int bits) {
    switch (f) {
        case OutFmt::I16: return bits <= 16;
        case OutFmt::I24: return bits <= 24;
        case OutFmt::I32: return true;
        default: return bits <= 24;   // float32 tiene 24 bits de mantisa
    }
}

struct OutCaps {                       // lo que declara el DAC (AudioDeviceInfo). Vacio = desconocido
    std::vector<int> rates;
    std::vector<OutFmt> formats;
    bool operator==(const OutCaps& o) const { return rates == o.rates && formats == o.formats; }
};

struct Candidate {
    int rate;
    OutFmt fmt;
    bool resampled;                    // rate != frecuencia de la pista
};

// Orden de intentos (todos con Exclusive):
//  1) frecuencia NATIVA, formatos sin perdida (entero nativo, luego contenedor mas ancho, luego float)
//  2) frecuencia DISTINTA que declara el DAC (primero las superiores), formato float/entero ancho + resampler
inline std::vector<Candidate> buildPlan(int srcRate, int srcBits, const OutCaps& caps, size_t maxAttempts = 8) {
    auto has = [](const auto& v, auto x) { return std::find(v.begin(), v.end(), x) != v.end(); };
    auto fmtOk = [&](OutFmt f) { return caps.formats.empty() || has(caps.formats, f); };

    std::vector<OutFmt> nativePref;
    if (srcBits <= 16) nativePref = {OutFmt::I16, OutFmt::I32, OutFmt::I24, OutFmt::Float};
    else if (srcBits <= 24) nativePref = {OutFmt::I32, OutFmt::I24, OutFmt::Float};
    else nativePref = {OutFmt::I32};

    std::vector<OutFmt> usable;
    for (OutFmt f : nativePref) if (fmtOk(f)) usable.push_back(f);
    if (usable.empty()) usable = nativePref;          // la lista del DAC no es fiable: probar igualmente

    std::vector<Candidate> plan;
    const bool nativeRateOk = caps.rates.empty() || has(caps.rates, srcRate);
    if (nativeRateOk) for (OutFmt f : usable) plan.push_back({srcRate, f, false});

    std::vector<int> rates = caps.rates.empty() ? std::vector<int>{48000, 96000} : caps.rates;
    std::vector<int> up, down;
    for (int r : rates) { if (r == srcRate) continue; (r > srcRate ? up : down).push_back(r); }
    std::sort(up.begin(), up.end());
    std::sort(down.begin(), down.end(), std::greater<int>());
    std::vector<int> chosen;
    for (size_t i = 0; i < up.size() && i < 2; ++i) chosen.push_back(up[i]);
    if (!down.empty()) chosen.push_back(down[0]);

    std::vector<OutFmt> rsUse;
    for (OutFmt f : {OutFmt::Float, OutFmt::I32, OutFmt::I24}) if (fmtOk(f)) rsUse.push_back(f);
    if (rsUse.empty() && fmtOk(OutFmt::I16)) rsUse.push_back(OutFmt::I16);
    if (rsUse.empty()) rsUse.push_back(OutFmt::I32);
    for (int r : chosen)
        for (size_t i = 0; i < rsUse.size() && i < 2; ++i) plan.push_back({r, rsUse[i], true});

    std::vector<Candidate> out;                        // sin duplicados y acotado
    for (const Candidate& c : plan) {
        bool dup = false;
        for (const Candidate& o : out) if (o.rate == c.rate && o.fmt == c.fmt) dup = true;
        if (!dup && out.size() < maxAttempts) out.push_back(c);
    }
    return out;
}
