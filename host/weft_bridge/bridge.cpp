// WeftBridge VST3 -- routes DAW audio to/from a running weft-host over a
// pair of named shared-memory rings (weft::AudioRing).
//
// This is the DAW-facing half of the "bridge" architecture (t5). The
// companion half is weft-host --audio ring, which creates the two rings
// (weft.bridge.in / weft.bridge.out by default) and pumps audio through the
// chain:
//
//     DAW -> WeftBridge --write-->  [ring weft.bridge.in]  (SPSC, interleaved f32)
//                                        |
//                                        v
//                              weft-host pump thread (runBlockLocked)
//                                        |
//     DAW <-- WeftBridge <--read-- [ring weft.bridge.out]
//
// Threading model (the important part):
//   * AudioRing::open() does CreateFileMappingW/MapViewOfFile (Win32) or
//     shm_open + a futex wait (POSIX). Neither is legal on an audio thread,
//     so the plugin attaches to the rings from a 1 s message-thread Timer
//     (timerCallback), NOT from processBlock. The host is a long-lived
//     process, so attach latency (a few ms, one-shot at plugin activation)
//     is acceptable.
//   * Once attached, the ring write/read path is lock-free (two atomic frame
//     cursors + memcpy), exactly as the host's own audio-thread pump uses it,
//     so processBlock may read/write the rings directly on the audio thread.
//
// DSP:
//   * "Bypass" param = 1  -> dry passthrough (ring untouched).
//   * "Bypass" = 0 and both rings attached:
//         de-interleave the DAW block -> ringIn.write()
//         ringOut.read() -> interleave into the DAW block.
//       If the host has not yet produced a full block (startup lag, or the
//       host is gone), the unread tail is zeroed. The ring itself is a FIFO,
//       so a short read never loses data -- it only adds latency.
//   * "Bypass" = 0 but rings NOT attached (host not running):
//         dry passthrough -- the buffer is left completely untouched, which
//         is what makes this plugin safe to drop into any session.
//
// Mono / channel handling: the host chain and the rings are fixed 2-channel
// (see examples/chain.json). A 1-channel DAW buffer is duplicated to stereo
// going in and down-mixed (mean) coming out; channels beyond 2 are passed
// through untouched.
//
// NOTE (Windows stale-ring caveat): a live named Win32 mapping cannot be
// deleted, so if weft-host crashes, ringOut keeps existing (valid() stays
// true) but stops producing frames; the plugin then zeros its output tail
// (silence) rather than misrouting audio. A host-side OSC heartbeat that
// could flip the status line is future work.

#include <cmath>
#include <string>
#include <vector>

#include "weft/ring.hpp"
#include "JuceHeader.h"

namespace weft_bridge {

// Default ring names. Must match the weft-host --ring PREFIX (default
// "weft.bridge"), which builds <prefix>.in / <prefix>.out. Kept as compile-
// time constants; a runtime override (plugin param or state) is future work.
static const char* kInName  = "weft.bridge.in";
static const char* kOutName = "weft.bridge.out";

static const int kHostChannels = 2;  // ring + host-chain channel count

class BridgeProcessor;
class BridgeEditor;  // forward decls (editor ctor takes BridgeProcessor&)

class BridgeProcessor : public juce::AudioProcessor,
                        private juce::Timer {
public:
    BridgeProcessor()
        : AudioProcessor(juce::AudioProcessor::BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo())
                             .withOutput("Output", juce::AudioChannelSet::stereo())),
          Timer() {
        // CRITICAL (JUCE 8.0.4): addParameter() transfers ownership of the
        // parameter to the AudioProcessor (it wraps the raw pointer in a
        // unique_ptr and the parameterTree deletes it in ~AudioProcessor).
        // Hold ONLY a raw pointer -- a second owner is a use-after-free.
        // See host/test_plugin/plugin.cpp for the full rationale.
        bypassParameter = new juce::AudioParameterBool("byp", "Bypass", false);
        addParameter(bypassParameter);

        // 1 s poll is plenty: the host's rings either exist (host is up) or
        // they appear within a second or two of the user starting the host.
        startTimerHz(1);
    }

    ~BridgeProcessor() override {
        stopTimer();
        ringIn_.close();   // safe on an invalid ring (no-op)
        ringOut_.close();
    }

    static juce::AudioProcessor* createPluginFilter() {
        return new BridgeProcessor();
    }

    const juce::String getName() const override { return "WeftBridge"; }
    bool hasEditor() const override { return true; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    // JUCE 8.0.4: these are pure-virtual on AudioProcessor (no defaults) --
    // must all be overridden. WeftBridge has no programs / no state.
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    // State / status ------------------------------------------------------
    // Called by the editor's own timer (message thread) to render the status
    // line. Written here by timerCallback (also message thread) -- no lock.
    juce::String getBridgeStatus() const { return status_; }

    // Audio ----------------------------------------------------------------
    void prepareToPlay(double sampleRate, int samplesPerBlock) override {
        hostSampleRate_ = sampleRate;
        // Force a fresh attach on (re)activation: if the host died since the
        // last session, close our (possibly stale) mappings so the timer
        // re-opens them.
        ringIn_.close();
        ringOut_.close();
        status_ = "waiting for weft-host";

        const size_t need = static_cast<size_t>(samplesPerBlock) * kHostChannels;
        if (inInter_.size() < need)  inInter_.resize(need);
        if (outInter_.size() < need) outInter_.resize(need);
    }

    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer,
                      juce::MidiBuffer&) override {
        // Dry passthrough: bypassed, or the host is not attached. Leaving the
        // buffer untouched is the safe default and matches the smoke plugin's
        // "passthrough = don't touch the buffer" contract.
        if (bypassParameter->get() || !ringIn_.valid() || !ringOut_.valid())
            return;

        const int numCh  = buffer.getNumChannels();
        const int frames = buffer.getNumSamples();
        if (frames <= 0)
            return;

        // 1) DAW block -> 2-channel interleaved -> ringIn.
        float* in = inInter_.data();
        if (numCh >= 2) {
            const float* L = buffer.getReadPointer(0);
            const float* R = buffer.getReadPointer(1);
            for (int s = 0; s < frames; ++s) {
                in[2 * s]     = L[s];
                in[2 * s + 1] = R[s];
            }
        } else {  // mono -> duplicate to stereo
            const float* M = buffer.getReadPointer(0);
            for (int s = 0; s < frames; ++s) {
                in[2 * s]     = M[s];
                in[2 * s + 1] = M[s];
            }
        }
        ringIn_.write(in, static_cast<size_t>(frames));

        // 2) ringOut -> 2-channel interleaved -> DAW block. A short read
        //    (host startup lag / host gone) zeroes the unread tail; the ring
        //    keeps the remainder for the next block (no data loss).
        const float* out = outInter_.data();
        const size_t got = ringOut_.read(outInter_.data(),
                                         static_cast<size_t>(frames));
        if (numCh >= 2) {
            float* L = buffer.getWritePointer(0);
            float* R = buffer.getWritePointer(1);
            for (int s = 0; s < frames; ++s) {
                if (s < static_cast<int>(got)) {
                    L[s] = out[2 * s];
                    R[s] = out[2 * s + 1];
                } else {
                    L[s] = 0.f;
                    R[s] = 0.f;
                }
            }
        } else {  // stereo -> mean to mono
            float* M = buffer.getWritePointer(0);
            for (int s = 0; s < frames; ++s) {
                M[s] = (s < static_cast<int>(got))
                           ? (out[2 * s] + out[2 * s + 1]) * 0.5f
                           : 0.f;
            }
        }
    }

    // Editor ---------------------------------------------------------------
    // Defined out-of-line (below BridgeEditor): `new BridgeEditor` needs the
    // complete editor type, which is only known after its definition.
    juce::AudioProcessorEditor* createEditor() override;

    // Timer ----------------------------------------------------------------
    void timerCallback() override {
        // Lazy attach. open() with a short timeout keeps the message thread
        // responsive: it returns as soon as the ring is ready or after 100 ms.
        if (!ringIn_.valid()) {
            std::string err;
            ringIn_ = weft::AudioRing::open(kInName, &err, 100);
        }
        if (!ringOut_.valid()) {
            std::string err;
            ringOut_ = weft::AudioRing::open(kOutName, &err, 100);
        }
        status_ = (ringIn_.valid() && ringOut_.valid())
                      ? "connected"
                      : "waiting for weft-host";
    }

private:
    // Owned by the AudioProcessor (see ctor comment) -- raw pointer.
    juce::AudioParameterBool* bypassParameter = nullptr;

    // Message-thread-owned; written by timerCallback, read by the editor.
    juce::String status_ = "waiting for weft-host";

    double hostSampleRate_ = 48000.0;

    // Attach state (valid()/close() are thread-agnostic here because the
    // attach happens on the message thread and the audio thread only ever
    // observes valid()==true and then uses the lock-free read/write path).
    weft::AudioRing ringIn_;
    weft::AudioRing ringOut_;

    // Audio-thread scratch (sized in prepareToPlay).
    std::vector<float> inInter_;
    std::vector<float> outInter_;
};

// --------------------------------------------------------------------------
// Minimal status editor: a title, a live status line, and a hint. Its own
// message-thread timer re-reads the processor's status string and repaints.
class BridgeEditor : public juce::AudioProcessorEditor,
                     private juce::Timer {
public:
    explicit BridgeEditor(BridgeProcessor& proc)
        : juce::AudioProcessorEditor(proc),
          proc_(proc),
          Timer() {
        // JUCE 8.0.4 API: Label::setText takes a NotificationType (not a
        // bool), and FontOptions has no .bolded()/.withColour() -- style is
        // set via the Font::bold style-flag ctor, colour via setColour().
        title_.setText("WeftBridge", juce::dontSendNotification);
        title_.setFont(juce::FontOptions(18.0f, juce::Font::bold));
        title_.setJustificationType(juce::Justification::centredLeft);
        status_.setText(proc_.getBridgeStatus(), juce::dontSendNotification);
        status_.setFont(juce::FontOptions(14.0f));
        status_.setJustificationType(juce::Justification::centredLeft);
        hint_.setText(
            "Start: weft-host <chain.json> --audio ring   (rings: weft.bridge.*)",
            juce::dontSendNotification);
        hint_.setFont(juce::FontOptions(11.0f));
        hint_.setColour(juce::Label::textColourId, juce::Colours::grey);
        hint_.setJustificationType(juce::Justification::centredLeft);
        hint_.setEditable(false);
        addAndMakeVisible(title_);
        addAndMakeVisible(status_);
        addAndMakeVisible(hint_);
        setResizable(false, false);
        setSize(420, 110);
        startTimerHz(2);
    }

    void paint(juce::Graphics& g) override {
        // JUCE 8.0.4 has no Colours::nearBlack and no Colour::withLevel --
        // use a fixed dark-grey hex for the editor background.
        g.fillAll(juce::Colour(0xff1e1e1e));
    }

    void timerCallback() override {
        status_.setText(proc_.getBridgeStatus(), juce::dontSendNotification);
    }

    void resized() override {
        title_.setBounds(16, 10, getWidth() - 32, 26);
        status_.setBounds(16, 40, getWidth() - 32, 22);
        hint_.setBounds(16, 68, getWidth() - 32, 20);
    }

private:
    BridgeProcessor& proc_;
    juce::Label title_;
    juce::Label status_;
    juce::Label hint_;
};

juce::AudioProcessorEditor* BridgeProcessor::createEditor() {
    return new BridgeEditor(*this);
}

}  // namespace weft_bridge

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return weft_bridge::BridgeProcessor::createPluginFilter();
}

// JucePlugin_CreateInstance is provided by the generated JuceHeader.h.
