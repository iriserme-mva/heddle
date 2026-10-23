#pragma once

#include <memory>
#include <string>
#include <vector>

#include "heddle/chain.hpp"
#include "heddle/param.hpp"

namespace juce {
class AudioPluginInstance;
}

namespace Steinberg {
namespace Vst {
class IAudioProcessor;
}
}

namespace heddle {

// JUCE-backed implementation of heddle::IPluginBackend.
//
// Loads VST3 plugin instances (the only format enabled in this build —
// see JUCE_PLUGINHOST_VST3) and maps their parameter surface onto the
// platform-agnostic heddle::Param / heddle::ParamSet model. Normalized
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

    // Push `frames` samples through the slot's plugin from EXPLICIT per-channel
    // pointers (in[ch] / out[ch] each point at `frames` contiguous samples of
    // channel ch). This is the form the in-block curve scheduler uses: the
    // host's planar work buffers have channel-stride == full block size, so a
    // sub-block read cannot use the flat `in + ch*frames` stride that process()
    // assumes — the caller hands in the true per-channel pointers instead.
    // Returns the number of frames processed, or -1 on error.
    int processChannels(const std::string& slotId, const float* const* in,
                        float* const* out, int channels, int frames);

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

    // ---- Raw VST3 in-block curve path (curve_resolution == 0) ---------------
    // The JUCE host wrapper latches ONE (param, value, offset-0) point per
    // process() call, so a smooth in-block curve cannot be expressed through
    // JUCE's processBlock. The VST3 host API, however, allows a full multi-point
    // IParameterChanges per block and the plugin is expected to interpolate
    // between queue points per-sample. When the plugin is a VST3 we can reach
    // its raw Vst::IAudioProcessor (via the JUCE ExtensionsVisitor) and drive
    // process() directly with a two-point queue (start @ offset 0, end @
    // offset S-1) -- one call per block. This is the DEFAULT in-block path.
    //
    // The raw pointer is resolved once on the message thread by resolveRaw()
    // (called from setProcessSpec after prepareToPlay, when the VST3 instance
    // is fully created). rawProcess()/hasRawProcessor() are then pure readers,
    // safe to call from the audio thread. A non-VST3 slot resolves to nullptr
    // and the caller falls back to the JUCE processBlock path (processChannels
    // with curveSubstep_).
    //
    // rawProcess() returns the number of frames processed, or -1 if the slot
    // has no raw IAudioProcessor (non-VST3) -- the caller falls back. in/out
    // are per-channel pointers each pointing at `frames` samples of channel ch
    // (the work buffers' channel stride is the full block size, so the caller
    // passes true per-channel pointers, as processChannels does). `changes`
    // is the block's RawParamChanges (2-point queue per active param), or
    // nullptr when no param is active this block. The plugin learns the sample
    // rate / block size from prepareToPlay (already issued), so this call
    // carries no rate (the SDK's ProcessData has no sample-rate field).
    int rawProcess(const std::string& slotId,
                   float** in, float** out,
                   int channels, int frames,
                   void* rawParamChanges);

    // True if the slot's plugin is a VST3 with a resolvable raw IAudioProcessor
    // (the in-block raw path is available). Pure read of a message-thread-resolved
    // pointer, safe from the audio thread.
    bool hasRawProcessor(const std::string& slotId) const;

    // Resolve every slot's raw Vst::IAudioProcessor (VST3 only). Message thread.
    void resolveRaw();

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
        // Raw VST3 accessors for the in-block curve path (curve_resolution==0).
        // audioProcessor_ is the plugin's own IAudioProcessor, reached through
        // JUCE's ExtensionsVisitor (getIComponentPtr()->queryInterface). We own
        // one COM reference (addRef'd on retrieval) and release it in releaseRaw().
        Steinberg::Vst::IAudioProcessor* audioProcessor_ = nullptr;
        // Bus channel counts (the plugin's real input/output width) at the time
        // the raw accessor was resolved; rawProcess sizes the two
        // AudioBusBuffers to these so an in-place plugin reads/writes every
        // channel and nothing past them.
        int rawInCh = 1;
        int rawOutCh = 1;
        // Defined in JuceBackend.cpp: release() needs the complete
        // Steinberg::Vst::IAudioProcessor type, but the header only forward-
        // declares it (to keep including JuceBackend.hpp cheap for the CLIs).
        void releaseRaw();
        Slot() = default;
        Slot(const std::string& i, const std::string& p,
             std::unique_ptr<juce::AudioPluginInstance> inst)
            : id(i), path(p), instance(std::move(inst)) {}
        // The user-declared ~Slot() suppresses the implicit move ctor/assign;
        // declare them explicitly (defined in JuceBackend.cpp) so slots_
        // (std::vector<Slot>) can reallocate. Moving transfers the unique_ptr
        // and the borrowed audioProcessor_ pointer as-is.
        Slot(Slot&&) noexcept;
        Slot& operator=(Slot&&) noexcept;
        ~Slot();
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
    };

    std::vector<Slot> slots_;
};

}  // namespace heddle
