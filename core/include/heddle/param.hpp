#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace heddle {

// Parameter semantics, mirroring VST2 kParam* flags / VST3 PType.
enum class ParamKind : uint8_t {
    Float = 0,  // continuous float
    Bool  = 1,  // binary (0/1)
    Enum  = 2,  // discrete stepped (kParamIsList / integer param)
    Chord = 3,  // bitfield (each step is an independent bit)
};

// One parameter of one plugin instance, as seen by the host.
//
// Normalized space is [0,1]. Raw space is [minValue, maxValue] with an
// optional step grid (stepCount == 0 means continuous).
struct Param {
    uint32_t id = 0;              // VST2: param index; VST3: 32-bit param id
    std::string title;            // human-readable name
    std::string units;            // unit string (e.g. "dB", "ms")
    float defaultValue = 0.f;     // normalized
    float value = 0.f;            // current, normalized
    float minValue = 0.f;         // raw
    float maxValue = 1.f;         // raw
    int stepCount = 0;            // 0 = continuous, >1 = number of discrete values
    ParamKind kind = ParamKind::Float;
    bool automatable = true;
    std::vector<std::string> listItems;  // labels for Enum/Chord

    static float clamp01(float v) {
        if (v < 0.f) return 0.f;
        if (v > 1.f) return 1.f;
        return v;
    }

    // raw -> normalized
    float toNormalized(float raw) const {
        const float span = maxValue - minValue;
        if (span <= 0.f) return 0.f;
        return clamp01((raw - minValue) / span);
    }

    // normalized -> raw (no step snapping)
    float fromNormalized(float norm) const {
        const float n = clamp01(norm);
        if (maxValue - minValue <= 0.f) return minValue;
        return minValue + n * (maxValue - minValue);
    }

    // normalized -> raw, snapped to the step grid when discrete
    float stepFromNormalized(float norm) const {
        const float n = clamp01(norm);
        if (stepCount <= 1) return fromNormalized(n);
        const float step = 1.f / static_cast<float>(stepCount - 1);
        const float snapped = std::round(n / step) * step;
        return fromNormalized(clamp01(snapped));
    }

    bool isDiscrete() const { return stepCount > 1; }
};

// The full, query-able parameter surface of one plugin in the chain.
// Built by the backend from VST2 getParams() / VST3 IEditController
// getParameterCount()/getParameterInfo() at load time, then kept live
// as plugin parameter values change at runtime.
struct ParamSet {
    std::string pluginId;    // chain slot id (e.g. "comp")
    std::string pluginName;  // e.g. "Compressor"
    std::string loadedPath;  // plugin file path this set was loaded from (hot-apply matching)
    std::vector<Param> params;

    const Param* find(uint32_t id) const {
        for (const auto& p : params)
            if (p.id == id) return &p;
        return nullptr;
    }
    Param* find(uint32_t id) {
        for (auto& p : params)
            if (p.id == id) return &p;
        return nullptr;
    }
    const Param* findByName(const std::string& name) const {
        for (const auto& p : params)
            if (p.title == name) return &p;
        return nullptr;
    }
    Param* findByName(const std::string& name) {
        for (auto& p : params)
            if (p.title == name) return &p;
        return nullptr;
    }
};

}  // namespace heddle
