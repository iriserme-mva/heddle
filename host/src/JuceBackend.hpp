#pragma once

#include <memory>
#include <string>
#include <vector>

#include "weft/chain.hpp"
#include "weft/param.hpp"

namespace juce {
class AudioPluginInstance;
}

namespace weft {

// JUCE-backed implementation of weft::IPluginBackend.
//
// Loads VST3 plugin instances (the only format enabled in this build —
// see JUCE_PLUGINHOST_VST3) and maps their parameter surface onto the
// platform-agnostic weft::Param / weft::ParamSet model. Normalized
// 0..1 space is used everywhere, matching the core library.
//
// All JUCE calls here happen on the calling thread, which main() makes
// the JUCE message thread via juce::initialiseJuce_GUI().
class JuceBackend : public IPluginBackend {
public:
    JuceBackend();
    ~JuceBackend() override;

    // IPluginBackend
    bool load(const SlotConfig& slot, ParamSet& outParams) override;
    bool setParams(const std::string& slotId,
                   const std::vector<std::pair<std::string, float>>& namedValues) override;
    bool setParam(const std::string& slotId, uint32_t id, float normValue) override;

    // Load a plugin by file path (convenience for the smoke CLI / tests).
    // Populates outParams with the full param surface; returns false (and
    // fills err) if the plugin could not be loaded or has no parameters.
    bool loadSlot(const std::string& slotId, const std::string& path,
                  ParamSet& outParams, std::string* err = nullptr);

    // Push one block through the slot's plugin.
    //   in:  input, float32, interleaved, [channel][frame]
    //   out: output, float32, interleaved, [channel][frame]
    // Returns the number of frames processed, or -1 on error.
    int process(const std::string& slotId, const float* in, float* out,
                int channels, int frames);

    // Re-prepare every loaded slot for the given offline process spec. loadSlot
    // prepares instances at a default rate (44100/512) so they exist before the
    // renderer knows the input file's sample rate; the renderer calls this once
    // it has read the file, so processing runs at the file's true rate/block.
    void setProcessSpec(double sampleRate, int samplesPerBlock);

    // Largest bus width (max of input and output channel count) across all
    // loaded slots, at least 1. The renderer sizes its shared processing buffer
    // to this so no slot — wider on either the input or output side — can
    // read/write past the buffer.
    int busWidth() const;

    // Number of output channels of the slot's plugin (totalNumOutputChannels).
    // The input channel count is returned if the slot is not loaded.
    int channelCount(const std::string& slotId, int fallback) const;

    int numLoadedSlots() const;

    // Read the live (normalized 0..1) value of a plugin parameter by model
    // param id, so the host can detect plugin-internal changes (automation,
    // an injected Bypass, an in-plugin editor change) that this backend does
    // not surface as host-change events. Returns -1 if the slot or param is
    // not found. Safe to call from the message thread: JUCE parameters are
    // thread-safe for reads.
    float paramValue(const std::string& slotId, uint32_t id) const;

private:
    struct Slot {
        std::string id;
        std::string path;
        std::unique_ptr<juce::AudioPluginInstance> instance;
    };

    std::vector<Slot> slots_;
};

}  // namespace weft
