#pragma once
#include <functional>
#include <string>
#include <vector>

#include "heddle/curve.hpp"
#include "heddle/param.hpp"

namespace heddle {

// One plugin slot in the chain.
struct SlotConfig {
    std::string id;             // unique within the chain (also used in OSC addr)
    std::string pluginPath;     // path to .vst3 / .vst
    std::string pluginId;       // optional VST3 class id / VST2 factory id (disambiguation)
    bool enabled = true;
    // Param name -> value: a static float (0..1) OR a time-series curve
    // (see curve.hpp / docs/CONFIG.md). Missing params keep plugin defaults.
    std::vector<std::pair<std::string, ParamSpec>> params;

    // The full spec for a param (static value or curve), or nullptr.
    const ParamSpec* paramSpec(const std::string& name) const {
        for (const auto& kv : params)
            if (kv.first == name) return &kv.second;
        return nullptr;
    }
    // Load-time value of a param: static value, or the curve's frame-0 value.
    // (Backward-compatible accessor; returns the fallback when absent.)
    float paramValue(const std::string& name, float fallback = 0.f) const {
        if (const ParamSpec* s = paramSpec(name)) return s->atStart();
        return fallback;
    }
    bool hasParam(const std::string& name) const {
        for (const auto& kv : params)
            if (kv.first == name) return true;
        return false;
    }
};

// Evaluate a slot's params at an absolute sample position, producing the
// concrete (name, value) pairs a backend's setParams() consumes. Static
// params evaluate to their value; curves interpolate at `frame`.
inline std::vector<std::pair<std::string, float>>
evalParams(const std::vector<std::pair<std::string, ParamSpec>>& params,
           int64_t frame) {
    std::vector<std::pair<std::string, float>> out;
    out.reserve(params.size());
    for (const auto& kv : params)
        out.emplace_back(kv.first, Param::clamp01(kv.second.at(frame)));
    return out;
}

// OSC server settings for the live app / bridge. When port == 0 the OSC
// server is disabled (offline render, or a backend that exposes no OSC).
// See docs/OSC.md for the address scheme.
struct OscConfig {
    int port = 0;                         // 0 = disabled; else UDP port to bind
    std::string bindHost = "127.0.0.1";   // interface the server binds to
};

struct ChainConfig {
    int sampleRate = 44100;
    int blockSize = 512;
    // In-block curve resolution.
    //   0 = default: WHOLE-QUEUE path. One raw process() per block carrying a
    //       two-point IParamValueQueue per active param (block-start value @
    //       offset 0, block-end value @ offset S-1). A compliant VST3
    //       linearly interpolates between the two points per-sample, so the
    //       rendered parameter follows the true chord -- smooth, not a
    //       staircase. Cheapest (one process() call per block) and still
    //       click-free via the 2-block lookahead ramp. Requires the plugin to
    //       be a VST3 (the host reaches its raw IAudioProcessor through JUCE);
    //       non-VST3 plugins fall back to the block-start latch.
    //   N (>0) = oversample: the block is sliced into N-sample segments, one
    //       control point (and one process() call) per segment. A finer
    //       staircase that converges to the chord; useful for plugins that do
    //       NOT interpolate between queue points (they latch per call).
    // The host CLI --substep N overrides this value when given.
    int curveResolution = 0;
    std::vector<SlotConfig> slots;
    OscConfig osc;
};

// Backend: implemented by the JUCE host (real plugins) or the CLI (offline
// plugins / dummy). The core never touches plugin SDKs directly.
class IPluginBackend {
public:
    virtual ~IPluginBackend() = default;

    // Load slot, enumerate all params into outParams (normalized defaults from
    // the plugin), then apply cfg.params on top. Returns false on load failure.
    virtual bool load(const SlotConfig& slot, ParamSet& outParams) = 0;

    // Apply a batch of parameter values (by name) to an already-loaded slot.
    // This is what makes a config's `params:` block actually reach the plugin:
    // load() only builds the model surface, the backend must push the values
    // into the live instance. Unknown names are skipped silently (they may
    // appear after a hot-swap to a different plugin).
    virtual bool setParams(const std::string& slotId,
                           const std::vector<std::pair<std::string, float>>& namedValues) = 0;

    // Set a parameter (normalized). Returns false if unknown id.
    virtual bool setParam(const std::string& slotId, uint32_t id, float normValue) = 0;
};

// The chain: owns config + per-slot ParamSets, delegates plugin work to the
// backend, and emits change callbacks (for OSC push / UI update).
class Chain {
public:
    using ChangeCallback = std::function<void(int slotIndex, const Param&)>;

    explicit Chain(IPluginBackend& backend) : backend_(backend) {}

    // Load all slots from cfg and apply configured params. Replaces state.
    bool load(const ChainConfig& cfg, std::string* err = nullptr);

    // Hot-apply: load missing slots, keep loaded ones, (re)apply all params.
    bool apply(const ChainConfig& cfg, std::string* err = nullptr);

    void resetToDefaults();

    // Reset one slot's param model to its plugin defaults (no-op if the index
    // is out of range). Like resetToDefaults() this only touches the model —
    // a live host must still push the values into the plugin instance.
    void resetSlot(int slotIndex);

    // Resolve a slot token: numeric index ("0") or slot id ("comp"). -1 if not found.
    int resolveSlot(const std::string& token) const;

    // Set by param name; returns current value or -1 if not found.
    float setParam(const std::string& slotToken, const std::string& name, float norm);

    // Set by param id; returns current value or -1 if not found.
    float setParamById(const std::string& slotToken, uint32_t id, float norm);

    // Model-only set by param id: records the value in the chain's param
    // surface WITHOUT latching the plugin (no backend_ call). The in-block
    // curve scheduler uses this so a scheduled OSC set reflects in /chain
    // queries while the audio thread alone drives the plugin's live value.
    // Returns the clamped value, or -1 if slot/param not found.
    float setParamModelById(int slotIndex, uint32_t id, float norm);

    const ParamSet* slot(int slotIndex) const {
        return (slotIndex >= 0 && slotIndex < (int)sets_.size()) ? &sets_[slotIndex] : nullptr;
    }
    const ChainConfig& config() const { return cfg_; }
    int numSlots() const { return (int)sets_.size(); }
    std::string slotName(int slotIndex) const {
        return (slotIndex >= 0 && slotIndex < (int)cfg_.slots.size())
                   ? cfg_.slots[slotIndex].id : std::string();
    }

    void onChange(ChangeCallback cb) { changeCb_ = std::move(cb); }

    // Called by the backend when the plugin itself changed a param value
    // (e.g. user touched the plugin UI, or automation inside the plugin).
    void onPluginParamChanged(const std::string& slotToken, uint32_t id, float norm) {
        const int idx = resolveSlot(slotToken);
        if (idx < 0 || !slot(idx) || !slot(idx)->find(id)) return;
        // Update the cached surface, then notify.
        auto* p = sets_[idx].find(id);
        if (!p) return;
        const float old = p->value;
        p->value = Param::clamp01(norm);
        if (p->value != old && changeCb_)
            changeCb_(idx, *p);
    }

private:
    IPluginBackend& backend_;
    ChainConfig cfg_;
    std::vector<ParamSet> sets_;
    ChangeCallback changeCb_;
};

}  // namespace heddle
