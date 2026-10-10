#include "JuceBackend.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include <juce_audio_processors/juce_audio_processors.h>

namespace weft {

namespace {

// Parse a JUCE parameter identifier into a 32-bit param id. JUCE's VST3 host
// exposes the raw Vst::ParamID (a uint32) as a *decimal* string, so this
// round-trips exactly. Non-numeric identifiers fall back to the slot index.
uint32_t paramIdFromJuce(const juce::String& id, uint32_t fallbackId) {
    if (id.isNotEmpty() && id.containsOnly("0123456789"))
        return static_cast<uint32_t>(id.getLargeIntValue());
    return fallbackId;
}

// Map a single JUCE parameter onto the weft::Param model.
weft::Param mapParam(const juce::AudioProcessorParameter* p, uint32_t fallbackId) {
    weft::Param param;

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
            param.kind = weft::ParamKind::Enum;
        } else {
            param.kind = weft::ParamKind::Float;  // stepped float, no labels
        }
    } else {
        param.stepCount = 0;
        param.kind = weft::ParamKind::Float;
    }

    return param;
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
        weft::Param* p = outParams.findByName(kv.first);
        if (p != nullptr)
            p->value = weft::Param::clamp01(kv.second.atStart());
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
            const float value = weft::Param::clamp01(kv.second);
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
    // A VST3 plugin is a bundle *directory* (WeftSmokePlugin.vst3/Contents/…);
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
    for (auto& slot : slots_) {
        if (slot.id != slotId || slot.instance == nullptr)
            continue;

        auto& inst = *slot.instance;
        const int inCh = std::max(1, inst.getTotalNumInputChannels());
        const int outCh = std::max(1, inst.getTotalNumOutputChannels());
        const int width = std::max(inCh, outCh);

        juce::AudioBuffer<float> buf(width, frames);
        for (int ch = 0; ch < inCh; ++ch)
            std::memcpy(buf.getWritePointer(ch), in + (size_t)ch * frames,
                        (size_t)frames * sizeof(float));
        for (int ch = inCh; ch < width; ++ch)
            buf.clear(ch, 0, frames);

        juce::MidiBuffer midi;
        inst.processBlock(buf, midi);

        for (int ch = 0; ch < outCh; ++ch)
            std::memcpy(out + (size_t)ch * frames, buf.getReadPointer(ch),
                        (size_t)frames * sizeof(float));
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
}

int JuceBackend::numLoadedSlots() const {
    return static_cast<int>(slots_.size());
}

}  // namespace weft
