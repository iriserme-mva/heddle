// Minimal test VST3 plugin for CI smoke + render tests.
//
// Two params, both continuous floats normalized 0..1:
//   "Gain"    (id "gain", default 1.0) — linear gain applied to the output.
//   "PWM pct" (id "pwmP", default 1.0) — duty cycle of a fixed-period
//                                         on/off gate (value 0..1, title
//                                         carries the percent meaning).
//
// DSP: out[n] = in[n] * Gain * gate[n], where the gate is 1.0 for the first
// round(duty * PERIOD) samples of each PERIOD-sample period and 0.0 for the
// rest. The gate phase is a member that survives across processBlock calls,
// so the waveform is continuous across the host's 512-frame blocks (and
// across prepareToPlay resets the phase restarts at 0, which is what makes
// the CI render assertions exact: a 2048-sample file at duty 0.5 is exactly
// two full periods, 512 of the 1024 samples in each ON).
//
// For a constant-amplitude input the mean output / mean input ratio is
// therefore exactly Gain * duty — the CI render job asserts that ratio,
// which proves BOTH config params reach the live instance (default is
// 1.0/1.0; if either were ignored the ratio would differ).

#include <cmath>
#include "JuceHeader.h"

namespace weft_smoke {

class SmokeProcessor : public juce::AudioProcessor {
public:
    // Gate period in samples. 1024 divides the CI file length (2048) and is
    // a multiple of the host block size (512), so period boundaries never
    // split a block unevenly in the tests — the ON count is exact.
    static constexpr int gatePeriod = 1024;

    SmokeProcessor()
        : AudioProcessor(juce::AudioProcessor::BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo())
                             .withOutput("Output", juce::AudioChannelSet::stereo())) {
        // CRITICAL (JUCE 8.0.4): addParameter() transfers ownership of the
        // parameter to the AudioProcessor — the parameterTree deletes it in
        // ~AudioProcessor (juce_AudioProcessor.cpp, addParameter wraps the raw
        // pointer in a unique_ptr). Do NOT also hold it in a unique_ptr member:
        // that double-ownership is a use-after-free that segfaults at teardown
        // of any instance (e.g. the throwaway component a VST3 host creates in
        // findAllTypesForFile) on every OS. Canonical JUCE pattern: raw pointer
        // member owned by the processor (see examples/Plugins/GainPluginDemo.h).
        gainParameter = new juce::AudioParameterFloat(
            "gain", "Gain", juce::NormalisableRange<float>(0.f, 1.f), 1.0f);
        addParameter(gainParameter);

        // Continuous range (NOT interval-stepped): a stepped NormalisableRange
        // would report stepCount != 0 in the VST3 ParameterInfo and break the
        // smoke assertion. Deliberately NO display lambda / parameterLabel:
        // (1) JUCE 8's AudioParameterFloat overloads take a 2-arg
        // stringFromValue (String(float,int)) or an Attributes object, not a
        // 1-arg lambda, so a "% display" lambda here won't compile; (2) a
        // parameterLabel is appended to the name the host sees via getName(),
        // which would break the host's exact title match on "PWM pct". The
        // title itself carries the unit; the value is the raw 0..1 duty.
        pwmParameter = new juce::AudioParameterFloat(
            "pwmP", "PWM pct", juce::NormalisableRange<float>(0.f, 1.f), 1.0f);
        addParameter(pwmParameter);
    }

    static juce::AudioProcessor* createPluginFilter() {
        return new SmokeProcessor();
    }

    const juce::String getName() const override { return "WeftSmokePlugin"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override {
        juce::ignoreUnused(sampleRate, samplesPerBlock);
        phase_ = 0;
    }

    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer,
                      juce::MidiBuffer&) override {
        // Read the params live: an offline renderer (and any live host) pushes
        // config values *after* prepareToPlay, so a cached copy would silently
        // ignore them. This is what makes the CI render assertion meaningful —
        // it proves the config's `params:` block actually reaches the plugin.
        const float gain = gainParameter->get();
        const float duty = juce::jlimit(0.f, 1.f, pwmParameter->get());
        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();
        const int onSamples = (int) std::lround (duty * (double) gatePeriod);

        for (int s = 0; s < numSamples; ++s) {
            const float g = (phase_ < onSamples) ? gain : 0.f;
            phase_ = (phase_ + 1) % gatePeriod;
            for (int c = 0; c < numChannels; ++c)
                buffer.getWritePointer (c) [s] *= g;
        }
    }

    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool isMidiEffect() const override { return false; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    // Owned by the AudioProcessor (see addParameter comment in ctor) — raw
    // pointer, never delete manually, never hold in a smart pointer.
    juce::AudioParameterFloat* gainParameter = nullptr;
    juce::AudioParameterFloat* pwmParameter = nullptr;

    // Gate phase, samples into the current PERIOD. Survives across
    // processBlock calls (the instance persists across render blocks /
    // live audio callbacks); reset to 0 in prepareToPlay.
    int phase_ = 0;
};

}  // namespace weft_smoke

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return weft_smoke::SmokeProcessor::createPluginFilter();
}

// JucePlugin_CreateInstance is provided by the generated JuceHeader.h.
