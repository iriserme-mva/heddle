#include "JuceBackend.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include <juce_audio_processors/juce_audio_processors.h>
// NOTE: no direct include of utilities/juce_ExtensionsVisitor.h here -- the
// umbrella header above already includes it (line 147), and that utility
// header has NO include guard (only #ifndef DOXYGEN), so a second include
// redefines juce::ExtensionsVisitor (C2011).

// Raw VST3 interfaces (IComponent / IAudioProcessor) used by the in-block
// curve path. The SDK root is on the include path (host/CMakeLists.txt); the
// SDK objects themselves are already compiled into juce::juce_audio_processors,
// so this is headers-only here -- no second copy, no ODR clash.
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "RawVst3.hpp"

namespace heddle {

namespace {

// Parse a JUCE parameter identifier into a 32-bit param id. JUCE's VST3 host
// exposes the raw Vst::ParamID (a uint32) as a *decimal* string, so this
// round-trips exactly. Non-numeric identifiers fall back to the slot index.
uint32_t paramIdFromJuce(const juce::String& id, uint32_t fallbackId) {
    if (id.isNotEmpty() && id.containsOnly("0123456789"))
        return static_cast<uint32_t>(id.getLargeIntValue());
    return fallbackId;
}

// Map a single JUCE parameter onto the heddle::Param model.
heddle::Param mapParam(const juce::AudioProcessorParameter* p, uint32_t fallbackId) {
    heddle::Param param;

    const auto* hosted =
        dynamic_cast<const juce::HostedAudioProcessorParameter*>(p);
    if (hosted != nullptr)
        param.id = paramIdFromJuce(hosted->getParameterID(), fallbackId);
    else
        param.id = fallbackId;

    param.title = p->getName(128).toStdString();
    param.units = p->getLabel().toStdString();
    param.defaultValue = p->getDefaultValue();
    param.value = p->getValue();
    param.automatable = p->isAutomatable();

    const auto* ranged = dynamic_cast<const juce::RangedAudioParameter*>(p);
    if (ranged != nullptr) {
        const juce::NormalisableRange<float> range = ranged->getNormalisableRange();
        param.minValue = range.start;
        param.maxValue = range.end;
    } else {
        param.minValue = 0.f;
        param.maxValue = 1.f;
    }

    // Discrete vs continuous. JUCE 8's VST3 host reports
    // getNumSteps() == getDefaultNumParameterSteps() for continuous params
    // and stepCount+1 for discrete ones; isDiscrete() normalizes that.
    const int steps = p->getNumSteps();
    if (p->isDiscrete() && steps > 1) {
        param.stepCount = steps;
        // Labels for discrete params. For raw VST3 this is ALWAYS empty:
        // VST3 ParameterInfo carries no value strings, and JUCE's
        // VST3Parameter (what the VST3 format actually returns to us)
        // returns {} from getAllValueStrings() — even for a choice param.
        // Only a non-format (in-process JUCE-native) host would expose
        // labels, which this backend never does.
        std::vector<std::string> items;
        const juce::StringArray sa = p->getAllValueStrings();
        for (int i = 0; i < sa.size(); ++i)
            items.push_back(sa[i].toStdString());
        if (!items.empty()) {
            param.listItems = std::move(items);
            param.kind = heddle::ParamKind::Enum;
        } else {
            param.kind = heddle::ParamKind::Float;  // stepped float, no labels
        }
    } else {
        param.stepCount = 0;
        param.kind = heddle::ParamKind::Float;
    }

    return param;
}

// Resolve the plugin's raw Vst::IAudioProcessor (only for VST3 instances).
// JUCE's AudioPluginInstance::getExtensions() dispatches to our visitor; the
// VST3Client hands back the same IComponent JUCE itself drives (getIComponentPtr),
// which we QueryInterface to IAudioProcessor. We keep one COM reference (the
// caller releases it via Slot::releaseRaw). Non-VST3 formats call visitUnknown
// and this returns nullptr -- the caller then uses the JUCE processBlock path.
Steinberg::Vst::IAudioProcessor* rawAudioProcessorOf(juce::AudioPluginInstance& inst) {
    // A visitor deriving from ExtensionsVisitor; the VST3 plugin instance's
    // getExtensions() builds its own VST3Client and calls visitVST3Client with
    // it. getIComponentPtr() is the same IComponent JUCE drives.
    struct V3 final : juce::ExtensionsVisitor {
        Steinberg::Vst::IComponent* component = nullptr;
        void visitVST3Client(const juce::ExtensionsVisitor::VST3Client& c) override {
            component = c.getIComponentPtr();
        }
    };
    V3 v3;
    inst.getExtensions(v3);
    if (v3.component == nullptr)
        return nullptr;

    // getIComponentPtr() hands a BORROWED pointer (JUCE's component.get(), no
    // addRef), so we must not release it. The queryInterface below returns a
    // fresh, independent reference on the IAudioProcessor interface -- that one
    // we DO own, and it is released later by Slot::releaseRaw().
    Steinberg::Vst::IAudioProcessor* audio = nullptr;
    const auto r =
        v3.component->queryInterface(Steinberg::Vst::IAudioProcessor::iid,
                                     reinterpret_cast<void**>(&audio));
    return (r == Steinberg::kResultOk) ? audio : nullptr;
}

}  // namespace

JuceBackend::JuceBackend() = default;
JuceBackend::~JuceBackend() = default;

bool JuceBackend::load(const SlotConfig& slot, ParamSet& outParams) {
    std::string err;
    if (!loadSlot(slot.id, slot.pluginPath, outParams, &err))
        return false;
    // Apply configured param values on top of plugin defaults (curve params
    // resolve to their frame-0 value). The model update is for
    // introspection; the push below is what actually changes the audio.
    for (const auto& kv : slot.params) {
        heddle::Param* p = outParams.findByName(kv.first);
        if (p != nullptr)
            p->value = heddle::Param::clamp01(kv.second.atStart());
    }
    return setParams(slot.id, evalParams(slot.params, 0));
}

bool JuceBackend::setParams(const std::string& slotId,
                            const std::vector<std::pair<std::string, float>>& namedValues) {
    for (auto& slot : slots_) {
        if (slot.id != slotId || slot.instance == nullptr)
            continue;
        const auto& params = slot.instance->getParameters();
        for (const auto& kv : namedValues) {
            const std::string name = kv.first;
            const float value = heddle::Param::clamp01(kv.second);
            for (int i = 0; i < params.size(); ++i) {
                if (params[i]->getName(128).toStdString() != name)
                    continue;
                const_cast<juce::AudioProcessorParameter*>(params[i])
                    ->setValueNotifyingHost(value);
                break;
            }
            // Unknown names are skipped: they may belong to a different plugin
            // after a hot-swap, and are kept in the config for that case.
        }
        return true;
    }
    return false;  // slot not found
}

bool JuceBackend::loadSlot(const std::string& slotId, const std::string& path,
                           ParamSet& outParams, std::string* err) {
    outParams = ParamSet{};

    const juce::File file(juce::String(path.c_str()));
    // A VST3 plugin is a bundle *directory* (HeddleSmokePlugin.vst3/Contents/…);
    // a VST2 plugin is a bare file (.so / .dll / .fx). Accept either and let
    // the format manager below decide which — fileMightContainThisPluginType +
    // findAllTypesForFile handle the file-vs-bundle distinction. A bare
    // existsAsFile() would wrongly reject every VST3 bundle.
    if (!file.exists()) {
        if (err) *err = "plugin not found: " + path;
        return false;
    }

    juce::AudioPluginFormatManager mgr;
    mgr.addDefaultFormats();

    juce::AudioPluginFormat* format = nullptr;
    for (int i = 0; i < mgr.getNumFormats(); ++i) {
        auto* f = mgr.getFormat(i);
        if (f != nullptr && f->fileMightContainThisPluginType(file.getFullPathName())) {
            format = f;
            break;
        }
    }
    if (format == nullptr) {
        if (err) *err = "no plugin format can load: " + path;
        return false;
    }

    juce::OwnedArray<juce::PluginDescription> descs;
    format->findAllTypesForFile(descs, file.getFullPathName());
    if (descs.size() == 0) {
        if (err) *err = "no plugin types found in: " + path;
        return false;
    }

    const juce::PluginDescription& desc = *descs[0];
    juce::String errStr;
    auto instance =
        mgr.createPluginInstance(desc, 44100.0, 512, errStr);
    if (instance == nullptr) {
        if (err) *err = "failed to create plugin instance: " + errStr.toStdString();
        return false;
    }

    instance->prepareToPlay(44100.0, 512);

    outParams.pluginId = slotId;
    outParams.pluginName = instance->getName().toStdString();
    outParams.loadedPath = path;

    const auto* params = instance->getParameters().data();
    const int n = instance->getParameters().size();
    for (int i = 0; i < n; ++i)
        outParams.params.push_back(mapParam(params[i], static_cast<uint32_t>(i)));

    if (outParams.params.empty()) {
        if (err) *err = "plugin has no parameters: " + path;
        return false;
    }

    slots_.push_back({slotId, path, std::move(instance)});
    return true;
}

bool JuceBackend::setParam(const std::string& slotId, uint32_t id,
                           float normValue) {
    for (auto& slot : slots_) {
        if (slot.id != slotId || slot.instance == nullptr)
            continue;
        const auto& params = slot.instance->getParameters();
        for (int i = 0; i < params.size(); ++i) {
            const auto* hosted =
                dynamic_cast<const juce::HostedAudioProcessorParameter*>(params[i]);
            if (hosted != nullptr &&
                paramIdFromJuce(hosted->getParameterID(), 0) == id) {
                const_cast<juce::AudioProcessorParameter*>(params[i])
                    ->setValueNotifyingHost(normValue);
                return true;
            }
        }
        return false;  // slot found but param id unknown
    }
    return false;  // slot not found
}

int JuceBackend::process(const std::string& slotId, const float* in, float* out,
                         int channels, int frames) {
    // Flat-pointer form (render.cpp, smoke CLI): the caller lays out one
    // (width, frames) planar buffer and passes its channel 0 base pointer;
    // per-channel stride is exactly `frames`, so this just adapts to
    // per-channel pointers and delegates to processChannels().
    const float* inPtrs[8];
    float* outPtrs[8];
    for (int ch = 0; ch < channels && ch < 8; ++ch) {
        inPtrs[ch] = (in != nullptr) ? in + (size_t)ch * frames : nullptr;
        outPtrs[ch] = (out != nullptr) ? out + (size_t)ch * frames : nullptr;
    }
    return processChannels(slotId, inPtrs, outPtrs, channels, frames);
}

int JuceBackend::processChannels(const std::string& slotId, const float* const* in,
                                 float* const* out, int channels, int frames) {
    for (auto& slot : slots_) {
        if (slot.id != slotId || slot.instance == nullptr)
            continue;

        auto& inst = *slot.instance;
        const int inCh = std::max(1, inst.getTotalNumInputChannels());
        const int outCh = std::max(1, inst.getTotalNumOutputChannels());
        const int width = std::max(inCh, outCh);
        // Per-channel pointer variant: one process() per sub-block slice.
        // The caller supplies (possibly offset) channel pointers into the
        // planar work buffers, so this copy only spans `frames` samples per
        // channel -- the plugin sees exactly the slice being rendered.
        juce::AudioBuffer<float> buf(width, frames);
        for (int ch = 0; ch < inCh && ch < channels; ++ch) {
            if (in != nullptr && in[ch] != nullptr)
                std::memcpy(buf.getWritePointer(ch), in[ch],
                            (size_t)frames * sizeof(float));
            else
                buf.clear(ch, 0, frames);
        }
        for (int ch = std::max(inCh, channels); ch < width; ++ch)
            buf.clear(ch, 0, frames);

        juce::MidiBuffer midi;
        inst.processBlock(buf, midi);

        for (int ch = 0; ch < outCh && ch < channels; ++ch) {
            if (out != nullptr && out[ch] != nullptr)
                std::memcpy(out[ch], buf.getReadPointer(ch),
                            (size_t)frames * sizeof(float));
        }
        return frames;
    }
    return -1;
}

int JuceBackend::channelCount(const std::string& slotId, int fallback) const {
    for (const auto& slot : slots_) {
        if (slot.id == slotId && slot.instance != nullptr)
            return std::max(1, slot.instance->getTotalNumOutputChannels());
    }
    return fallback;
}

// Out-of-line Slot raw teardown: needs the complete Steinberg::Vst::IAudioProcessor
// type (only forward-declared in the header).
JuceBackend::Slot::~Slot() { releaseRaw(); }
JuceBackend::Slot::Slot(Slot&& other) noexcept
    : id(std::move(other.id)),
      path(std::move(other.path)),
      instance(std::move(other.instance)),
      audioProcessor_(other.audioProcessor_),
      rawInCh(other.rawInCh),
      rawOutCh(other.rawOutCh) {
    other.audioProcessor_ = nullptr;  // moved-from holds no COM reference
    other.rawInCh = 1;
    other.rawOutCh = 1;
}
JuceBackend::Slot& JuceBackend::Slot::operator=(Slot&& other) noexcept {
    if (this != &other) {
        releaseRaw();
        id          = std::move(other.id);
        path        = std::move(other.path);
        instance    = std::move(other.instance);
        audioProcessor_ = other.audioProcessor_;
        rawInCh           = other.rawInCh;
        rawOutCh          = other.rawOutCh;
        other.audioProcessor_ = nullptr;
        other.rawInCh = 1;
        other.rawOutCh = 1;
    }
    return *this;
}
void JuceBackend::Slot::releaseRaw() {
    if (audioProcessor_ != nullptr) {
        audioProcessor_->release();
        audioProcessor_ = nullptr;
    }
}

bool JuceBackend::hasRawProcessor(const std::string& slotId) const {
    for (const auto& slot : slots_) {
        if (slot.id == slotId)
            return slot.audioProcessor_ != nullptr;
    }
    return false;
}

void JuceBackend::resolveRaw() {
    // Message thread, after prepareToPlay: the VST3 instance is fully created
    // and its ExtensionsVisitor answers visitVST3Client. Re-resolve every slot
    // (releases any stale reference from a previous setProcessSpec/hot-apply).
    for (auto& slot : slots_) {
        slot.releaseRaw();
        if (slot.instance == nullptr)
            continue;
        slot.audioProcessor_ = rawAudioProcessorOf(*slot.instance);
        if (slot.audioProcessor_ != nullptr) {
            slot.rawInCh  = std::max(1, slot.instance->getTotalNumInputChannels());
            slot.rawOutCh = std::max(1, slot.instance->getTotalNumOutputChannels());
        }
    }
}

int JuceBackend::rawProcess(const std::string& slotId, float** in,
                            float** out, int channels, int frames,
                            void* rawParamChanges) {
    for (const auto& slot : slots_) {
        if (slot.id != slotId)
            continue;
        if (slot.audioProcessor_ == nullptr)
            return -1;  // not a VST3 (or not resolved) -> caller falls back

        // rawv::rawProcess (RawVst3.hpp) builds the two AudioBusBuffers, a
        // single-bus realtime ProcessData, and hands `rawParamChanges` as
        // inputParameterChanges (nullptr when no param is active). The plugin
        // learned sample rate / block size from prepareToPlay. Buffers are
        // sized to the plugin's real bus widths so an in-place plugin reads
        // and writes every channel and nothing past them. `channels` is the
        // host's work-buffer width (== min bus width by construction).
        (void) channels;
        heddle::rawv::RawParamChanges* changes =
            static_cast<heddle::rawv::RawParamChanges*>(rawParamChanges);
        const auto r = heddle::rawv::rawProcess(
            slot.audioProcessor_,
            0u,                                   // sample rate: no-op (not a ProcessData field)
            (uint32_t) frames,
            in,
            (uint32_t) slot.rawInCh,
            out,
            (uint32_t) slot.rawOutCh,
            changes);
        return (r == Steinberg::kResultOk) ? frames : -1;
    }
    return -1;
}

int JuceBackend::busWidth() const {
    int width = 1;
    for (const auto& slot : slots_) {
        if (slot.instance == nullptr)
            continue;
        width = std::max(width, slot.instance->getTotalNumInputChannels());
        width = std::max(width, slot.instance->getTotalNumOutputChannels());
    }
    return width;
}

void JuceBackend::setProcessSpec(double sampleRate, int samplesPerBlock) {
    for (auto& slot : slots_)
        if (slot.instance != nullptr)
            slot.instance->prepareToPlay(sampleRate, samplesPerBlock);
    // Now that every VST3 instance is prepared (setupProcessing issued), the
    // ExtensionsVisitor answers visitVST3Client -- resolve the raw
    // IAudioProcessor pointers the in-block curve path (curve_resolution==0)
    // uses. Message thread only; the audio thread reads them as plain pointers.
    resolveRaw();
}

int JuceBackend::numLoadedSlots() const {
    return static_cast<int>(slots_.size());
}

float JuceBackend::paramValue(const std::string& slotId, uint32_t id) const {
    for (const auto& slot : slots_) {
        if (slot.id != slotId || slot.instance == nullptr)
            continue;
        const auto params = slot.instance->getParameters();
        for (int i = 0; i < params.size(); ++i) {
            const auto* hosted =
                dynamic_cast<const juce::HostedAudioProcessorParameter*>(params[i]);
            if (hosted == nullptr)
                continue;
            if (paramIdFromJuce(hosted->getParameterID(), 0) == id)
                return params[i]->getValue();
        }
        return -1.f;  // slot found but param id unknown
    }
    return -1.f;  // slot not found
}

}  // namespace heddle
