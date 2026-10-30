// HeddleBridge VST3 -- routes DAW audio to/from a running heddle-host over a
// pair of named shared-memory rings (heddle::AudioRing) AND acts as the
// self-contained GUI launcher for a chain.
//
// This is the DAW-facing half of the "bridge" architecture. The companion
// half is heddle-host --audio ring, which creates the two rings (heddle.bridge.in
// / heddle.bridge.out by default) and pumps audio through the chain:
//
//     DAW -> HeddleBridge --write-->  [ring heddle.bridge.in]  (SPSC, interleaved f32)
//                                        |
//                                        v
//                              heddle-host pump thread (runBlockLocked)
//                                        |
//     DAW <-- HeddleBridge <--read-- [ring heddle.bridge.out]
//
// The editor is a LAUNCHER, not just a status box:
//   * Drag .vst3 bundles (or .vst/.dll/.so plugins) from Explorer anywhere onto
//     the window, or use "Add plugin..." to browse. Drop a .json config to
//     load a chain into the editor.
//   * The chain order is the list order (slot 0 -> slot 1 -> ...), with
//     up / down / remove / rename controls.
//   * "Save config..." writes a Heddle chain JSON (heddle::config::dump) so
//     the user never hand-edits config.
//   * "Start host" spawns `heddle-host <config> --audio ring` hidden
//     (CreateProcessW CREATE_NO_WINDOW -- no terminal). The host exe is
//     auto-located (scan of the install roots + walk-up from the plugin,
//     see locateHostExe) with a Browse override + last-used persistence.
//   * The live status line keeps its old role: "waiting for heddle-host"
//     until the rings appear, then "connected".
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
// deleted, so if heddle-host crashes, ringOut keeps existing (valid() stays
// true) but stops producing frames; the plugin then zeros its output tail
// (silence) rather than misrouting audio. A host-side OSC heartbeat that
// could flip the status line is future work.

#include <cmath>
#include <string>
#include <vector>

#include "heddle/config.hpp"
#include "heddle/ring.hpp"
#include "JuceHeader.h"

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace heddle_bridge {

// Default ring names. Must match the heddle-host --ring PREFIX (default
// "heddle.bridge"), which builds <prefix>.in / <prefix>.out. Kept as compile-
// time constants; a runtime override (plugin param or state) is future work.
static const char* kInName  = "heddle.bridge.in";
static const char* kOutName = "heddle.bridge.out";

static const int kHostChannels = 2;  // ring + host-chain channel count

// Launcher defaults. The OSC port matches the host's own default (9001), so a
// freshly-saved config always starts with a reachable control port. (The host
// ABORTS startup if the OSC port is already taken -- see setupOsc() in
// host_main.cpp.) Sample rate is pre-filled from the DAW's actual rate captured
// in prepareToPlay, because the rings are created by the host at the CONFIG's
// rate, so a mismatch is a real audio-rate disagreement the user should see.
static const int    kDefaultSampleRate = 48000;
static const int    kDefaultOscPort    = 9001;
static const char*  kAppName           = "HeddleBridge";
static const char*  kSettingsFileName  = "settings.json";
static const char*  kDefaultConfigDir  = "Heddle";

// The host exe name, per platform.
#if JUCE_WINDOWS
static const char* kHostExeName = "heddle-host.exe";
#else
static const char* kHostExeName = "heddle-host";
#endif

class BridgeProcessor;
class BridgeEditor;  // forward decls (editor ctor takes BridgeProcessor&)

// ---------------------------------------------------------------------------
// Launcher model: the ordered list of chain slots the user is building.
// Lives on the message thread only (the editor reads/writes it from UI
// callbacks). A slot maps 1:1 to a heddle::SlotConfig when we dump config.
struct SlotEntry {
    juce::String id;        // unique slot id (also the OSC address token)
    juce::String path;      // absolute path to the .vst3 bundle / .dll / .vst
};

// ---------------------------------------------------------------------------
// BridgeProcessor: audio ring routing + a message-thread status string the
// editor renders. No state persistence here -- the editor owns that.
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

    const juce::String getName() const override { return "HeddleBridge"; }
    bool hasEditor() const override { return true; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    // JUCE 8.0.4: these are pure-virtual on AudioProcessor (no defaults) --
    // must all be overridden. HeddleBridge has no programs / no state.
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    // State / status ------------------------------------------------------
    // Read by the editor's own timer (message thread). Written here by
    // timerCallback (also message thread) -- no lock.
    juce::String getBridgeStatus() const { return status_; }
    // The DAW's current sample rate (set in prepareToPlay) so the launcher can
    // pre-fill the config's sample_rate with the session's actual rate.
    int getDaWSampleRate() const { return daWSampleRate_; }

    // Audio ----------------------------------------------------------------
    void prepareToPlay(double sampleRate, int samplesPerBlock) override {
        daWSampleRate_ = static_cast<int>(std::lround(sampleRate));
        // Force a fresh attach on (re)activation: if the host died since the
        // last session, close our (possibly stale) mappings so the timer
        // re-opens them.
        ringIn_.close();
        ringOut_.close();
        status_ = "waiting for heddle-host";

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
            ringIn_ = heddle::AudioRing::open(kInName, &err, 100);
        }
        if (!ringOut_.valid()) {
            std::string err;
            ringOut_ = heddle::AudioRing::open(kOutName, &err, 100);
        }
        status_ = (ringIn_.valid() && ringOut_.valid())
                      ? "connected"
                      : "waiting for heddle-host";
    }

private:
    // Owned by the AudioProcessor (see ctor comment) -- raw pointer.
    juce::AudioParameterBool* bypassParameter = nullptr;

    // Message-thread-owned; written by timerCallback, read by the editor.
    juce::String status_ = "waiting for heddle-host";

    // DAW sample rate. prepareToPlay runs on the audio thread; the editor
    // reads it on the message thread and tolerates a stale value (an int is
    // atomic-sized on every supported ABI and only moves at activation).
    int daWSampleRate_ = kDefaultSampleRate;

    // Attach state (valid()/close() are thread-agnostic here because the
    // attach happens on the message thread and the audio thread only ever
    // observes valid()==true and then uses the lock-free read/write path).
    heddle::AudioRing ringIn_;
    heddle::AudioRing ringOut_;

    // Audio-thread scratch (sized in prepareToPlay).
    std::vector<float> inInter_;
    std::vector<float> outInter_;
};

// ---------------------------------------------------------------------------
// Host-exe discovery. The MSI installs heddle-host.exe under
// C:\Program Files\Heddle_vX\bin\ while the plugin .vst3 goes to a separate
// (often C:\Program Files\Common Files\VST3) tree, so auto-locate scans the
// install roots AND walks up from the plugin's own module path. A user browse
// override + last-used persistence (BridgeSettings.hostExe) always wins.
static juce::File locateHostExe() {
#if JUCE_WINDOWS
    // 1) Walk up from our own module (the plugin .dll inside the .vst3
    //    bundle, or a dev build where bin/ sits a few levels up).
    {
        wchar_t buf[MAX_PATH] = {};
        const DWORD n = ::GetModuleFileNameW(::GetModuleHandleW(nullptr),
                                             buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            juce::File d = juce::File(juce::String(buf, n)).getParentDirectory();
            for (int i = 0; i < 8; ++i) {
                const juce::File direct = d.getChildFile(kHostExeName);
                if (direct.existsAsFile()) return direct;
                const juce::File inBin = d.getChildFile("bin").getChildFile(kHostExeName);
                if (inBin.existsAsFile()) return inBin;
                d = d.getParentDirectory();
            }
        }
    }
    // 2) Scan Program Files (x86) for Heddle_v* install roots -> bin\.
    for (const juce::String pf :
         { "C:\\Program Files", "C:\\Program Files (x86)" }) {
        const juce::File root(pf);
        if (!root.exists()) continue;
        const juce::RangedDirectoryIterator sentinel;  // default-ctor = end sentinel
        for (auto it = juce::RangedDirectoryIterator(root, /*isRecursive*/ false,
                                                     "Heddle*",
                                                     juce::File::findDirectories);
             it != sentinel; ++it) {
            const juce::File cand = it->getFile()
                .getChildFile("bin").getChildFile(kHostExeName);
            if (cand.existsAsFile()) return cand;
        }
    }
#else
    // POSIX best-effort: current working directory. Primary target is Windows.
    const juce::File inCwd =
        juce::File::getCurrentWorkingDirectory().getChildFile(kHostExeName);
    if (inCwd.existsAsFile()) return inCwd;
#endif
    return juce::File();
}

// ---------------------------------------------------------------------------
// Editor-side launcher settings, persisted as a tiny key=value text file in
// the user's home dir (no app-data quirks, works under DAW UAC too).
struct BridgeSettings {
    juce::String hostExe;      // user override for the host exe ("" = auto)
    juce::String lastConfig;   // last config path (Save/Start reuse it)
    int sampleRate = kDefaultSampleRate;
    int oscPort = kDefaultOscPort;

    static juce::File file() {
        return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
            .getChildFile(kAppName).getChildFile(kSettingsFileName);
    }
    void load() {
        const juce::File f = file();
        if (!f.existsAsFile()) return;
        const juce::StringArray lines =
            juce::StringArray::fromLines(f.loadFileAsString().trim());
        for (int i = 0; i < lines.size(); ++i) {
            const juce::String& line = lines[i];
            const juce::int64 eq = line.indexOf("=");
            if (eq <= 0) continue;
            const juce::String k = line.substring(0, eq).trim();
            const juce::String v = line.substring((juce::int64)eq + 1).trim();
            if (k == "hostExe")            hostExe = v;
            else if (k == "lastConfig")    lastConfig = v;
            else if (k == "sampleRate") { int x = v.getIntValue(); if (x > 0) sampleRate = x; }
            else if (k == "oscPort")    { int x = v.getIntValue(); if (x > 0) oscPort = x; }
        }
    }
    bool store() const {
        juce::String out;
        out << "hostExe="    << hostExe << juce::newLine;
        out << "lastConfig=" << lastConfig << juce::newLine;
        out << "sampleRate=" << sampleRate << juce::newLine;
        out << "oscPort="    << oscPort << juce::newLine;
        const juce::File f = file();
        if (!f.getParentDirectory().exists()) f.getParentDirectory().createDirectory();
        return f.replaceWithText(out);
    }
};

// Build the heddle ChainConfig from the launcher model and serialise it.
// Returns the JSON text, or an empty string + *err on validation failure
// (round-tripped through the canonical parser so the editor reports the SAME
// error the host would -- see core/src/config.cpp).
static juce::String buildConfigJson(const juce::Array<SlotEntry>& slots,
                                    int sampleRate, int oscPort,
                                    juce::String* err) {
    heddle::ChainConfig cfg;
    cfg.sampleRate = sampleRate > 0 ? sampleRate : kDefaultSampleRate;
    cfg.blockSize  = 1024;
    cfg.osc.port   = oscPort;
    cfg.osc.bindHost = "127.0.0.1";
    for (const auto& s : slots) {
        heddle::SlotConfig sc;
        sc.id = s.id.toStdString();
        sc.pluginPath = s.path.toStdString();
        sc.enabled = true;
        cfg.slots.push_back(std::move(sc));
    }
    const std::string text = heddle::config::dump(cfg);
    heddle::ChainConfig check;
    std::string perr;
    if (!heddle::config::parse(text, check, &perr)) {
        if (err) *err = perr;
        return {};
    }
    return juce::String(text.c_str());
}

// Parse existing Heddle chain JSON text into the launcher model (drop/load).
static bool parseConfigText(const juce::String& text,
                            juce::Array<SlotEntry>& slots,
                            int& rate, int& port,
                            juce::String& err) {
    heddle::ChainConfig cfg;
    std::string perr;
    if (!heddle::config::parse(text.toStdString(), cfg, &perr)) {
        err = perr;
        return false;
    }
    slots.clear();
    for (const auto& s : cfg.slots) {
        SlotEntry e;
        e.id = juce::String(s.id.c_str());
        e.path = juce::String(s.pluginPath.c_str());
        slots.add(e);
    }
    if (cfg.sampleRate > 0) rate = cfg.sampleRate;
    if (cfg.osc.port > 0) port = cfg.osc.port;
    return true;
}

// ---------------------------------------------------------------------------
// ListBoxModel: renders the ordered module list (chain order).
class SlotListModel : public juce::ListBoxModel {
public:
    explicit SlotListModel(juce::Array<SlotEntry>* slots) : slots_(slots) {}
    int getNumRows() override {
        return slots_ ? static_cast<int>(slots_->size()) : 0;
    }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h,
                          bool selected) override {
        if (slots_ == nullptr || row < 0 || row >= static_cast<int>(slots_->size()))
            return;
        const SlotEntry& s = (*slots_)[row];
        if (selected)
            g.fillAll(juce::Colour(0xff2d4d6b));
        else if (row % 2 == 0)
            g.fillAll(juce::Colour(0xff232323));
        else
            g.fillAll(juce::Colour(0xff1c1c1c));
        g.setColour(juce::Colour(0xffd8e6f0));
        const juce::String idx = juce::String(row + 1) + ".  ";
        const juce::String name = s.id + "    " + juce::File(s.path).getFileName();
        juce::Font f(13.0f);
        g.setFont(f);
        const int idxW = f.getStringWidth(idx);
        g.drawText(idx, 8, 0, idxW + 4, h, juce::Justification::left);
        g.drawText(name, 12 + idxW, 0, w - idxW - 20, h,
                   juce::Justification::left, 1);
    }
    juce::String getNameForRow(int row) override {
        if (slots_ == nullptr || row < 0 || row >= static_cast<int>(slots_->size()))
            return {};
        return (*slots_)[row].id;
    }
private:
    juce::Array<SlotEntry>* slots_;
};

// ---------------------------------------------------------------------------
// A tiny modal rename dialog: a text field + OK/Cancel. Built as a plain
// Component added to the desktop and driven with enterModalState/runModalLoop
// (the canonical JUCE modal pattern; JUCE_MODAL_LOOPS_PERMITTED is set for
// this target because the launcher runs on the host message thread).
class RenameBox : public juce::Component {
public:
    explicit RenameBox(const juce::String& initialValue) {
        setOpaque(true);
        editor_.setText(initialValue, juce::dontSendNotification);
        editor_.setFont(juce::FontOptions(14.0f));
        editor_.setBounds(12, 34, 256, 26);
        addAndMakeVisible(editor_);

        ok_ = new juce::TextButton("OK");
        cancel_ = new juce::TextButton("Cancel");
        for (juce::TextButton* b : { ok_, cancel_ }) {
            b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2b2f36));
            b->setColour(juce::TextButton::textColourOffId, juce::Colours::white);
            b->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
            addAndMakeVisible(b);
        }
        ok_->onClick = [this] {
            result_ = editor_.getText();
            exitModalState(kOk);
        };
        cancel_->onClick = [this] { exitModalState(kCancel); };
        setSize(280, 110);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff242424));
        g.setColour(juce::Colours::lightgrey);
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText("Slot id", 12, 10, 256, 20, juce::Justification::left);
    }
    void resized() override {
        ok_->setBounds(72, 68, 90, 28);
        cancel_->setBounds(170, 68, 98, 28);
    }
    // Returns the new id, or an empty string on Cancel.
    static juce::String ask(const juce::String& current) {
        RenameBox box(current);
        box.addToDesktop(0);
        box.enterModalState(true);
        const int r = box.runModalLoop();
        return r == kOk ? box.result_ : juce::String();
    }
private:
    static constexpr int kOk = 1;
    static constexpr int kCancel = 0;
    juce::TextEditor editor_;
    juce::TextButton *ok_ = nullptr, *cancel_ = nullptr;
    juce::String result_;
};

// ---------------------------------------------------------------------------
// The launcher editor. FileDragAndDropTarget (drop .vst3/.vst/.dll/.so plugins
// or .json configs anywhere on the editor) + ListBox (the ordered module list)
// + sample-rate/OSC rows + host-exe row + Save config / Start host buttons.
// All state lives on the message thread.
class BridgeEditor : public juce::AudioProcessorEditor,
                     private juce::Timer,
                     public juce::FileDragAndDropTarget {
public:
    explicit BridgeEditor(BridgeProcessor& proc)
        : juce::AudioProcessorEditor(proc),
          proc_(proc),
          model_(&slots_),
          list_() {
        settings_.load();

        title_.setText("HeddleBridge", juce::dontSendNotification);
        title_.setFont(juce::FontOptions(18.0f, juce::Font::bold));
        title_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(title_);

        status_.setText(proc_.getBridgeStatus(), juce::dontSendNotification);
        status_.setFont(juce::FontOptions(14.0f));
        status_.setJustificationType(juce::Justification::centredLeft);
        status_.setColour(juce::Label::textColourId, juce::Colours::dodgerblue);
        addAndMakeVisible(status_);

        dropHint_.setText(
            "Drag .vst3 plugins (or a .json config) anywhere onto this window.",
            juce::dontSendNotification);
        styleDim(dropHint_);
        addAndMakeVisible(dropHint_);

        list_.setModel(&model_);
        list_.setRowHeight(26);
        list_.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff161616));
        list_.setColour(juce::ListBox::textColourId, juce::Colours::white);
        addAndMakeVisible(list_);

        srLabel_.setText("Sample rate (Hz)", juce::dontSendNotification);
        styleDim(srLabel_);
        addAndMakeVisible(srLabel_);
        srValue_.setText(juce::String(settings_.sampleRate), juce::dontSendNotification);
        styleValue(srValue_);
        addAndMakeVisible(srValue_);

        oscLabel_.setText("OSC port", juce::dontSendNotification);
        styleDim(oscLabel_);
        addAndMakeVisible(oscLabel_);
        oscValue_.setText(juce::String(settings_.oscPort), juce::dontSendNotification);
        styleValue(oscValue_);
        addAndMakeVisible(oscValue_);

        exeLabel_.setText("heddle-host exe", juce::dontSendNotification);
        styleDim(exeLabel_);
        addAndMakeVisible(exeLabel_);
        exePath_.setEditable(false);
        exePath_.setFont(juce::FontOptions(11.0f));
        exePath_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(exePath_);
        browseExe_ = makeButton("Browse...");
        browseExe_->onClick = [this] { onBrowseExe(); };
        addAndMakeVisible(browseExe_);

        // Pre-fill the sample rate from the DAW when it has already started,
        // so a freshly-saved config matches the running session.
        const int daw = proc_.getDaWSampleRate();
        if (daw >= 30000 && daw <= 200000) {
            settings_.sampleRate = daw;
            srValue_.setText(juce::String(daw), juce::dontSendNotification);
        }

        hostExe_ = settings_.hostExe;
        if (hostExe_.isEmpty()) refreshHostExe();
        updateExePathLabel();

        addBtn_     = makeButton("Add plugin...");
        removeBtn_  = makeButton("Remove");
        upBtn_      = makeButton("Up");
        downBtn_    = makeButton("Down");
        renameBtn_  = makeButton("Rename");
        saveBtn_    = makeButton("Save config...");
        startBtn_   = makeButton("Start host");
        addBtn_->onClick     = [this] { onAdd(); };
        removeBtn_->onClick  = [this] { onRemove(); };
        upBtn_->onClick      = [this] { onMove(-1); };
        downBtn_->onClick    = [this] { onMove(+1); };
        renameBtn_->onClick  = [this] { onRename(); };
        saveBtn_->onClick    = [this] { onSave(); };
        startBtn_->onClick   = [this] { onStart(); };
        for (juce::TextButton* b : { addBtn_, removeBtn_, upBtn_, downBtn_,
                                     renameBtn_, saveBtn_, startBtn_ })
            addAndMakeVisible(b);
        startBtn_->setColour(juce::TextButton::buttonColourId,
                             juce::Colour(0xff1f6b3a));

        setResizable(false, false);
        setSize(620, 470);
        startTimerHz(2);
    }

    ~BridgeEditor() override { settings_.store(); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff1e1e1e));
        g.setColour(dragOver_ ? juce::Colour(0x6600e5ff) : juce::Colour(0xff2a2a2a));
        g.drawRect(list_.getBounds().expanded(4));
    }

    void timerCallback() override {
        status_.setText(proc_.getBridgeStatus(), juce::dontSendNotification);
    }

    void resized() override {
        const int W = getWidth();
        const int margin = 16;
        title_.setBounds(margin, 10, W - 2 * margin, 26);
        status_.setBounds(margin, 40, W - 2 * margin, 22);
        dropHint_.setBounds(margin, 66, W - 2 * margin, 18);
        list_.setBounds(margin, 90, W - 2 * margin, 200);

        int y = 300;
        const int bh = 28;
        int bx = margin;
        addBtn_->setBounds(bx, y, 104, bh);   bx += 108;
        removeBtn_->setBounds(bx, y, 74, bh);  bx += 78;
        upBtn_->setBounds(bx, y, 48, bh);      bx += 52;
        downBtn_->setBounds(bx, y, 58, bh);    bx += 62;
        renameBtn_->setBounds(bx, y, 74, bh);  y += bh + 12;

        srLabel_.setBounds(margin, y, 120, 24);
        srValue_.setBounds(margin + 120, y, 90, 24);
        oscLabel_.setBounds(margin + 230, y, 80, 24);
        oscValue_.setBounds(margin + 310, y, 70, 24);
        y += 24 + 12;

        exeLabel_.setBounds(margin, y, 120, 24);
        exePath_.setBounds(margin + 120, y, W - 2 * margin - 120 - 90, 24);
        browseExe_->setBounds(W - margin - 80, y, 80, 24);
        y += 24 + 12;

        saveBtn_->setBounds(margin, y, 180, bh);
        startBtn_->setBounds(W - margin - 210, y, 210, bh);
    }

    // ---- FileDragAndDropTarget ------------------------------------------
    bool isInterestedInFileDrag(const juce::StringArray& files) override {
        for (const juce::String& f : files) {
            const juce::String ext = juce::File(f).getFileExtension().toLowerCase();
            if (isPluginPath(ext) || ext == ".json")
                return true;
        }
        return false;
    }
    void fileDragEnter(const juce::StringArray&, int, int) override {
        dragOver_ = true;
        repaint();
    }
    void fileDragExit(const juce::StringArray&) override {
        dragOver_ = false;
        repaint();
    }
    void filesDropped(const juce::StringArray& files, int, int) override {
        dragOver_ = false;
        for (const juce::String& f : files) {
            const juce::File file(f);
            const juce::String ext = file.getFileExtension().toLowerCase();
            if (ext == ".json") {
                loadConfigFromFile(file);
            } else if (isPluginPath(ext)) {
                addSlotFromPath(file.getFullPathName());
            }
        }
        list_.updateContent();
        repaint();
    }

private:
    // ---- small helpers ---------------------------------------------------
    static void styleDim(juce::Label& l) {
        l.setFont(juce::FontOptions(12.0f));
        l.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        l.setJustificationType(juce::Justification::centredLeft);
        l.setEditable(false);
    }
    static void styleValue(juce::Label& l) {
        l.setFont(juce::FontOptions(13.0f));
        l.setColour(juce::Label::textColourId, juce::Colours::white);
        l.setJustificationType(juce::Justification::centredLeft);
        l.setEditable(false);
    }
    juce::TextButton* makeButton(const juce::String& text) {
        juce::TextButton* b = new juce::TextButton(text);
        b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2b2f36));
        b->setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        b->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        return b;
    }
    static bool isPluginPath(const juce::String& ext) {
        return ext == ".vst3" || ext == ".vst" || ext == ".dll" || ext == ".so";
    }
    void showWarn(const juce::String& msg) {
        juce::AlertWindow::showMessageBox(juce::MessageBoxIconType::WarningIcon,
                                          kAppName, msg);
    }
    int selectedRow() const { return list_.getSelectedRow(); }
    bool idInUse(const juce::String& id) const {
        for (const auto& s : slots_)
            if (s.id == id) return true;
        return false;
    }

    // ---- slot management -------------------------------------------------
    void addSlotFromPath(const juce::String& path) {
        const juce::File file(path);
        if (!file.exists()) return;
        for (const auto& s : slots_)
            if (s.path == file.getFullPathName()) return;  // no duplicates
        juce::String id = file.getFileNameWithoutExtension().replace(" ", "");
        if (id.isEmpty()) id = "slot";
        const juce::String base = id;
        int n = 1;
        while (idInUse(id)) { id = base + juce::String(n); ++n; }
        SlotEntry e;
        e.id = id;
        e.path = file.getFullPathName();
        slots_.add(e);
    }
    void onAdd() {
        juce::FileChooser chooser("Add a plugin", juce::File(),
                                  "*.vst3;*.vst;*.dll;*.so");
        if (!chooser.showDialog(juce::FileBrowserComponent::openMode
                                | juce::FileBrowserComponent::canSelectFiles,
                                nullptr))
            return;
        const juce::File result = chooser.getResult();
        if (result.exists()) {
            addSlotFromPath(result.getFullPathName());
            list_.updateContent();
        }
    }
    void onRemove() {
        const int r = selectedRow();
        if (r < 0) return;
        slots_.remove(r);
        list_.updateContent();
    }
    void onMove(int delta) {
        const int r = selectedRow();
        if (r < 0) return;
        const int to = r + delta;
        if (to < 0 || to >= slots_.size()) return;
        const SlotEntry e = slots_[r];
        slots_.remove(r);
        slots_.insert(to, e);
        list_.updateContent();
    }
    void onRename() {
        const int r = selectedRow();
        if (r < 0) return;
        const juce::String newId = RenameBox::ask(slots_[r].id).trim();
        if (newId.isEmpty())
            return;  // cancelled (or cleared) -- keep the old id
        if (idInUse(newId)) {
            showWarn("Slot id must be unique.");
            return;
        }
        slots_[r].id = newId;
        list_.updateContent();
    }

    // ---- config save / load ----------------------------------------------
    juce::String generateConfig(juce::String* err) {
        return buildConfigJson(slots_, settings_.sampleRate,
                               settings_.oscPort, err);
    }
    void onSave() {
        if (slots_.isEmpty()) {
            showWarn("Add at least one plugin first (drag .vst3 files onto this window).");
            return;
        }
        juce::File workDir;
        if (settings_.lastConfig.isNotEmpty()
            && juce::File(settings_.lastConfig).getParentDirectory().exists()) {
            workDir = juce::File(settings_.lastConfig).getParentDirectory();
        } else {
            juce::File docs =
                juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                    .getChildFile(kDefaultConfigDir);
            if (!docs.exists()) docs.createDirectory();
            workDir = docs;
        }
        juce::FileChooser chooser("Save Heddle chain config", workDir, "*.json");
        if (!chooser.showDialog(juce::FileBrowserComponent::saveMode
                                | juce::FileBrowserComponent::canSelectFiles
                                | juce::FileBrowserComponent::warnAboutOverwriting,
                                nullptr))
            return;
        juce::File result = chooser.getResult();
        if (result.getFileExtension() != ".json")
            result = result.withFileExtension("json");
        juce::String err;
        const juce::String text = generateConfig(&err);
        if (text.isEmpty()) { showWarn("Could not build config:\n" + err); return; }
        if (!result.replaceWithText(text)) {
            showWarn("Could not write " + result.getFullPathName());
            return;
        }
        settings_.lastConfig = result.getFullPathName();
        settings_.store();
        saveBtn_->setButtonText("Saved: " + result.getFileName());
        juce::Timer::callAfterDelay(1500, [this] {
            saveBtn_->setButtonText("Save config...");
        });
    }
    void loadConfigFromFile(const juce::File& file) {
        if (!file.existsAsFile()) return;
        juce::String err;
        int rate = settings_.sampleRate, port = settings_.oscPort;
        if (!parseConfigText(file.loadFileAsString(), slots_, rate, port, err)) {
            showWarn("Could not parse config:\n" + err);
            return;
        }
        settings_.sampleRate = rate;
        srValue_.setText(juce::String(rate), juce::dontSendNotification);
        settings_.oscPort = port;
        oscValue_.setText(juce::String(port), juce::dontSendNotification);
        settings_.lastConfig = file.getFullPathName();
        list_.updateContent();
    }

    // ---- host-exe resolution / start --------------------------------------
    void refreshHostExe() {
        if (!settings_.hostExe.isEmpty()
            && juce::File(settings_.hostExe).existsAsFile()) {
            hostExe_ = settings_.hostExe;
            return;
        }
        const juce::File found = locateHostExe();
        hostExe_ = found.existsAsFile() ? found.getFullPathName() : juce::String();
    }
    void updateExePathLabel() {
        const bool ok = hostExe_.isNotEmpty()
                        && juce::File(hostExe_).existsAsFile();
        exePath_.setText(ok ? hostExe_
                            : "(not found -- use Browse... to point at heddle-host[.exe])",
                         juce::dontSendNotification);
        exePath_.setColour(juce::Label::textColourId,
                           ok ? juce::Colours::lightgrey : juce::Colours::orange);
    }
    void onBrowseExe() {
#if JUCE_WINDOWS
        const juce::String filter = "*.exe";
#else
        const juce::String filter = "*";
#endif
        juce::FileChooser chooser("Choose the heddle-host executable",
                                  juce::File(), filter);
        if (!chooser.showDialog(juce::FileBrowserComponent::openMode
                                | juce::FileBrowserComponent::canSelectFiles,
                                nullptr))
            return;
        const juce::File result = chooser.getResult();
        if (!result.existsAsFile()) return;
        settings_.hostExe = result.getFullPathName();
        settings_.store();
        hostExe_ = settings_.hostExe;
        updateExePathLabel();
    }
    void onStart() {
        if (slots_.isEmpty()) {
            showWarn("Add at least one plugin first (drag .vst3 files onto this window).");
            return;
        }
        // 1) Persist the current chain: last config path, else
        //    ~/Documents/Heddle/chain.json.
        juce::File cfgFile;
        if (settings_.lastConfig.isNotEmpty()
            && juce::File(settings_.lastConfig).getParentDirectory().exists()) {
            cfgFile = juce::File(settings_.lastConfig);
        } else {
            const juce::File docs =
                juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                    .getChildFile(kDefaultConfigDir);
            if (!docs.exists()) docs.createDirectory();
            cfgFile = docs.getChildFile("chain.json");
        }
        juce::String err;
        const juce::String text = generateConfig(&err);
        if (text.isEmpty()) { showWarn("Could not build config:\n" + err); return; }
        if (!cfgFile.replaceWithText(text)) {
            showWarn("Could not write config:\n" + cfgFile.getFullPathName());
            return;
        }
        settings_.lastConfig = cfgFile.getFullPathName();
        settings_.store();

        // 2) Resolve the host exe.
        if (hostExe_.isEmpty() || !juce::File(hostExe_).existsAsFile()) {
            settings_.hostExe.clear();
            refreshHostExe();
            updateExePathLabel();
        }
        if (hostExe_.isEmpty() || !juce::File(hostExe_).existsAsFile()) {
            showWarn("heddle-host[.exe] not found.\nUse Browse... to point at the "
                     "executable (it is installed in Heddle's bin folder).");
            return;
        }

        // 3) Spawn: heddle-host <config> --audio ring   (hidden, no console).
        const juce::String full = "\""
            + juce::File(hostExe_).getFullPathName() + "\" \""
            + cfgFile.getFullPathName() + "\" --audio ring";
        bool launched = false;
#if JUCE_WINDOWS
        {
            // CreateProcessW's lpCommandLine is a MUTABLE wide buffer; copy
            // out of the JUCE String (its wide form is valid while `full`
            // lives) and null-terminate it explicitly.
            std::wstring wcmd(full.toWideCharPointer());
            wcmd.push_back(L'\0');
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            const BOOL ok = ::CreateProcessW(nullptr, &wcmd[0], nullptr, nullptr,
                                             FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                             &si, &pi);
            launched = (ok != FALSE);
            if (launched) {
                ::CloseHandle(pi.hThread);
                ::CloseHandle(pi.hProcess);
            } else {
                showWarn("Failed to start heddle-host (Win32 error "
                         + juce::String((int)::GetLastError()) + ").");
            }
        }
#else
        {
            // POSIX detached spawn (best effort -- primary target is Windows).
            const std::string cmdS = full.toStdString();
            const pid_t pid = ::fork();
            if (pid == 0) {
                ::execlp("/bin/sh", "sh", "-c", cmdS.c_str(), (char*)nullptr);
                _exit(127);
            }
            launched = (pid > 0);
            if (!launched) showWarn("Failed to start heddle-host (fork).");
        }
#endif
        if (!launched) return;
        startBtn_->setEnabled(false);
        startBtn_->setButtonText("Started");
        juce::Timer::callAfterDelay(1800, [this] {
            startBtn_->setButtonText("Start host");
            startBtn_->setEnabled(true);
        });
    }

    // ---- members ----------------------------------------------------------
    BridgeProcessor& proc_;
    juce::Array<SlotEntry> slots_;
    SlotListModel model_;
    juce::ListBox list_;
    BridgeSettings settings_;
    juce::String hostExe_;
    bool dragOver_ = false;

    juce::Label title_;
    juce::Label status_;
    juce::Label dropHint_;
    juce::Label srLabel_;
    juce::Label srValue_;
    juce::Label oscLabel_;
    juce::Label oscValue_;
    juce::Label exeLabel_;
    juce::Label exePath_;
    juce::TextButton *addBtn_ = nullptr, *removeBtn_ = nullptr, *upBtn_ = nullptr,
        *downBtn_ = nullptr, *renameBtn_ = nullptr, *saveBtn_ = nullptr,
        *startBtn_ = nullptr, *browseExe_ = nullptr;
};

juce::AudioProcessorEditor* BridgeProcessor::createEditor() {
    return new BridgeEditor(*this);
}

}  // namespace heddle_bridge

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return heddle_bridge::BridgeProcessor::createPluginFilter();
}

// JucePlugin_CreateInstance is provided by the generated JuceHeader.h.
