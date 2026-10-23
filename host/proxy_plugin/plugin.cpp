// heddle-proxy (passthrough VST3) — routes one OSC surface to up to 5
// Heddle targets. Audio passes through UNCHANGED (this plugin adds no DSP);
// the OSC router (heddle_core's ProxyEngine) runs on a juce::Timer on the
// message thread, so the audio callback is never touched. See docs/PROXY.md.
//
// The editor exposes the full routing table:
//   * 3-state mode button: OFF / ONLY-CONTROL / FULL
//   * listen port (the port the surface / TouchOSC sends to)
//   * up to 5 targets (ip / port / enabled)
//   * up to 8 routes (surface addr -> target idx / slot / param)
//   * DISCOVER: query a target via the Heddle OSC protocol and list its
//     slots/params; double-click a line to insert a route.
//   * live counters (received / forwarded / dropped)
//
// The routing table is persisted in the plugin state (getStateInformation /
// setStateInformation) so it follows the DAW's save.
#include <cmath>
#include <string>

#include "JuceHeader.h"
#include "heddle/proxy.hpp"
#include "heddle/proxy_engine.hpp"

namespace heddle_proxy {

// The OSC router types live in heddle (the JUCE-free core); pull them into
// this namespace so the processor/editor can name them unqualified.
using namespace heddle;

// JUCE String from a std::string (String::fromUTF8 takes const char*; the
// std::string ctor is not used so the char-set is explicit).
inline juce::String jstr(const std::string& x) { return juce::String::fromUTF8(x.data()); }

// Forward decl: the editor is defined after the processor (createEditor()
// returns a new one).
class ProxyEditor;

// ===========================================================================
// Processor
// ===========================================================================
class ProxyProcessor : public juce::AudioProcessor,
                       public juce::Timer {
public:
    ProxyProcessor()
        : AudioProcessor(BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo())
                             .withOutput("Output", juce::AudioChannelSet::stereo())) {
        // Default (no state) config: listen on 9100, only-control, empty
        // table. The engine is started lazily from the message thread (see
        // timerCallback) so a throwaway instance created during VST3 factory
        // enumeration never binds a socket or touches the audio thread.
        cfg_.listenPort = 9100;
        cfg_.listenHost = "127.0.0.1";
        cfg_.mode = "only-control";
        startTimerHz(200);  // pump OSC ~5ms (non-blocking) on the msg thread
    }

    ~ProxyProcessor() override {
        stopTimer();
        engine_.stop();
    }

    static juce::AudioProcessor* createPluginFilter() { return new ProxyProcessor(); }

    const juce::String getName() const override { return "HeddleProxy"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    // ------------------------------------------------------------------
    // Audio: pure passthrough. The router lives on the message thread; this
    // callback must stay free of any engine access.
    // ------------------------------------------------------------------
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {
        // Nothing: input == output, unchanged.
    }

    bool hasEditor() const override { return true; }
    // Defined out-of-line (after ProxyEditor is complete) — the editor class
    // is only forward-declared at this point.
    juce::AudioProcessorEditor* createEditor() override;

    // ------------------------------------------------------------------
    // Message-thread OSC pump. Starts the engine on the first tick (after the
    // MessageManager is up and state has been applied), then drains.
    // ------------------------------------------------------------------
    void timerCallback() override {
        if (!engine_.running())
            ensureStarted();
        engine_.pump();
    }

    void ensureStarted() {
        if (engine_.running()) return;
        std::string err;
        if (!engine_.start(cfg_, &err))
            statusMsg_ = "start failed: " + err;
        else
            statusMsg_ = "running";
    }

    // (Re)bind with the current config. Called by the editor on APPLY / mode
    // change / discovered-route insertion.
    void commitConfig(const juce::String& label) {
        std::string err;
        if (!engine_.start(cfg_, &err)) {
            statusMsg_ = "start failed: " + err;
        } else {
            statusMsg_ = label + " (" + cfg_.mode + ")";
        }
    }

    ProxyConfig cfg_;
    juce::String statusMsg_ = "not started";
    ProxyEngine engine_;

    // ------------------------------------------------------------------
    // State: the routing table as JSON (round-trips through proxy::dump).
    // ------------------------------------------------------------------
    void getStateInformation(juce::MemoryBlock& dest) override {
        const juce::String j = jstr(proxy::dump(cfg_));
        dest.replaceAll(j.toRawUTF8(), j.getNumBytesAsUTF8());
    }
    void setStateInformation(const void* data, int sizeInBytes) override {
        if (data == nullptr || sizeInBytes <= 0) return;
        const juce::String j(reinterpret_cast<const char*>(data), sizeInBytes);
        std::string err;
        ProxyConfig next;
        if (proxy::parse(j.toStdString(), next, &err)) {
            cfg_ = std::move(next);
            statusMsg_ = "state loaded";
            // Re-apply immediately (idempotent; a no-op bind if already up).
            std::string serr;
            if (!engine_.start(cfg_, &serr))
                statusMsg_ = "state loaded (start failed: " + serr + ")";
        }
    }

private:
    // dest.replace overloads: replace(j.getData(),0,bytes) is the documented
    // MemoryBlock::replace(const void*, size_t start, size_t length) — it
    // overwrites the whole block when start=0 and length covers the old size.
};

// ===========================================================================
// Discovered-params list model (double-click inserts a route)
// ===========================================================================
class DiscoveredModel : public juce::ListBoxModel {
public:
    struct Entry {
        int targetIdx = 0;
        ProxyDiscovered::Param p;
    };
    using InsertCb = std::function<void(int /*targetIdx*/, const ProxyDiscovered::Param&)>;

    int getNumRows() override { return (int) entries_.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int, int height, bool) override {
        if (row < 0 || row >= (int) entries_.size()) return;
        const auto& e = entries_[row];
        const juce::String text =
            juce::String("t" + juce::String(e.targetIdx) + "  /chain/" +
                         jstr(e.p.slot) + "/" +
                         jstr(e.p.param) +
                         "   (id " + juce::String(e.p.paramId) + ")");
        g.setFont(height * 0.68f);
        g.drawText(text, 4, 2, 2000, height, juce::Justification::left);
    }
    // JUCE 8.0.4: the double-click hook is listBoxItemDoubleClicked(row, e)
    // (there is no doubleClick(int) — that is TableListBoxModel).
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override {
        if (row < 0 || row >= (int) entries_.size() || !insert_) return;
        insert_(entries_[row].targetIdx, entries_[row].p);
    }
    // JUCE 8.0.4 ListBoxModel has no begin/endUpdate/updateAllRows — repaint
    // the owning list instead.
    void refresh(const std::vector<Entry>& e, juce::ListBox* list) {
        entries_ = e;
        if (list != nullptr) list->repaint();
    }
    void setInsertCallback(InsertCb cb) { insert_ = std::move(cb); }

private:
    std::vector<Entry> entries_;
    InsertCb insert_;
};

// ===========================================================================
// Editor
// ===========================================================================
class ProxyEditor : public juce::AudioProcessorEditor,
                    public juce::Timer {
public:
    explicit ProxyEditor(ProxyProcessor& owner)
        : juce::AudioProcessorEditor(owner), owner_(owner) {
        setSize(780, 600);

        title_ = new juce::Label("t", "Heddle Proxy — OSC fan-out");
        title_->setFont(juce::FontOptions(15.0f, juce::Font::bold));
        title_->setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(title_);

        status_ = new juce::Label("s", owner_.statusMsg_.toRawUTF8());
        status_->setFont(juce::FontOptions(12.0f, juce::Font::bold));
        status_->setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(status_);

        // Mode 3-state button (OFF / ONLY-CONTROL / FULL).
        mode_ = new juce::TextButton("mode");
        mode_->setButtonText("MODE: " + modeLabel(owner_.cfg_.mode));
        mode_->onClick = [this] {
            cycleMode();
            owner_.commitConfig("mode -> " + modeLabel(owner_.cfg_.mode).toStdString());
            mode_->setButtonText("MODE: " + modeLabel(owner_.cfg_.mode));
        };
        addAndMakeVisible(mode_);

        apply_ = new juce::TextButton("apply");
        apply_->setButtonText("APPLY");
        apply_->onClick = [this] {
            readInputs();
            owner_.commitConfig("applied");
            refreshStatus();
        };
        addAndMakeVisible(apply_);

        listenLbl_ = new juce::Label("ll", "listen port");
        listenLbl_->setFont(juce::FontOptions(12.0f));
        addAndMakeVisible(listenLbl_);
        listenEd_ = new juce::TextEditor("lpe");
        listenEd_->setText(juce::String(owner_.cfg_.listenPort));
        listenEd_->setJustification(juce::Justification::centredLeft);
        addAndMakeVisible(listenEd_);

        buildTargets();
        buildRoutes();
        buildDiscover();

        // Poll the live counters + status.
        startTimerHz(4);
        refreshStatus();
    }

    ~ProxyEditor() override = default;

    void timerCallback() override { refreshStatus(); }

    void resized() override {
        const int W = getWidth();
        title_->setBounds(12, 8, W - 12, 20);
        status_->setBounds(W - 320, 10, 308, 18);

        mode_->setBounds(12, 34, 180, 28);
        apply_->setBounds(200, 34, 90, 28);
        listenLbl_->setBounds(300, 38, 78, 20);
        listenEd_->setBounds(380, 36, 70, 22);

        // Targets block
        const int t0 = 70;
        targetsHdr_->setBounds(12, t0, 300, 16);
        const int tRowH = 26;
        for (int i = 0; i < ProxyConfig::kMaxTargets; ++i) {
            const int y = t0 + 20 + i * tRowH;
            targetEn_[i]->setBounds(12, y, 60, tRowH - 4);
            targetIp_[i]->setBounds(78, y, 170, tRowH - 6);
            targetPort_[i]->setBounds(254, y, 60, tRowH - 6);
        }

        // Routes block
        const int r0 = t0 + 20 + ProxyConfig::kMaxTargets * tRowH + 8;
        routesHdr_->setBounds(12, r0, 400, 16);
        const int rRowH = 26;
        const int xSrc = 12, xTgt = 168, xSlot = 236, xParam = 356, xClr = 500;
        for (int i = 0; i < kMaxRoutes; ++i) {
            const int y = r0 + 20 + i * rRowH;
            routeSrc_[i]->setBounds(xSrc, y, 150, rRowH - 6);
            routeTgt_[i]->setBounds(xTgt, y, 64, rRowH - 4);
            routeSlot_[i]->setBounds(xSlot, y, 114, rRowH - 6);
            routeParam_[i]->setBounds(xParam, y, 146, rRowH - 6);
            routeClr_[i]->setBounds(xClr, y, 26, rRowH - 4);
        }

        // Discover block
        const int d0 = r0 + 20 + kMaxRoutes * rRowH + 6;
        discBtn_->setBounds(12, d0, 110, 28);
        discTgt_->setBounds(128, d0, 90, 26);
        discList_->setBounds(12, d0 + 34, 470, 60);
        discLbl_->setBounds(490, d0 + 34, W - 500, 56);
    }

private:
    static constexpr int kMaxRoutes = 8;

    static juce::String modeLabel(const std::string& m) {
        if (m == "off") return "OFF";
        if (m == "full") return "FULL";
        return "ONLY-CONTROL";
    }
    void cycleMode() {
        if (owner_.cfg_.mode == "off") owner_.cfg_.mode = "only-control";
        else if (owner_.cfg_.mode == "only-control") owner_.cfg_.mode = "full";
        else owner_.cfg_.mode = "off";
    }

    // Copy editor field values into owner_.cfg_ (targets + routes + listen).
    void readInputs() {
        std::string err;
        int port = listenEd_->getText().getIntValue();
        if (port > 0 && port <= 65535) owner_.cfg_.listenPort = port;

        // Targets: rebuild from the rows (a row is present if it has an ip or
        // port value).
        std::vector<ProxyTarget> ts;
        for (int i = 0; i < ProxyConfig::kMaxTargets && (int) ts.size() < ProxyConfig::kMaxTargets; ++i) {
            const juce::String ip = targetIp_[i]->getText().trim();
            const int p = targetPort_[i]->getText().getIntValue();
            if (ip.isEmpty() && p <= 0) continue;
            ProxyTarget t;
            t.ip = ip.toStdString();
            t.port = p;
            t.enabled = targetEn_[i]->getToggleState();
            if (!t.ip.empty() && t.port > 0) ts.push_back(std::move(t));
        }
        owner_.cfg_.targets = std::move(ts);

        // Routes: keep the target index but clamp it to the rebuilt target
        // count (removing a target may invalidate a route's target index).
        std::vector<ProxyRoute> rs;
        for (int i = 0; i < kMaxRoutes; ++i) {
            const juce::String src = routeSrc_[i]->getText().trim();
            const juce::String slot = routeSlot_[i]->getText().trim();
            const juce::String param = routeParam_[i]->getText().trim();
            if (src.isEmpty() && slot.isEmpty() && param.isEmpty()) continue;
            ProxyRoute r;
            r.srcAddr = src.startsWith("/") ? src.toStdString() : ("/" + src).toStdString();
            int ti = routeTgt_[i]->getSelectedId() - 1;
            if (ti < 0) ti = 0;
            if (ti >= (int) owner_.cfg_.targets.size())
                ti = std::max(0, (int) owner_.cfg_.targets.size() - 1);
            r.targetIdx = ti;
            r.slot = slot.toStdString();
            r.param = param.toStdString();
            if (!r.slot.empty() && !r.param.empty())
                rs.push_back(std::move(r));
        }
        owner_.cfg_.routes = std::move(rs);
        (void) err;
    }

    void buildTargets() {
        targetsHdr_ = new juce::Label("th", "Targets  (surface -> these Heddle instances)");
        targetsHdr_->setFont(juce::FontOptions(12.0f, juce::Font::bold));
        addAndMakeVisible(targetsHdr_);
        for (int i = 0; i < ProxyConfig::kMaxTargets; ++i) {
            targetEn_[i] = new juce::ToggleButton("te" + juce::String(i));
            targetEn_[i]->setButtonText("on");
            targetEn_[i]->setToggleState(owner_.cfg_.targets.size() > i ? owner_.cfg_.targets[i].enabled : true,
                                         juce::dontSendNotification);
            addAndMakeVisible(targetEn_[i]);
            targetIp_[i] = new juce::TextEditor("ti" + juce::String(i));
            if (owner_.cfg_.targets.size() > i)
                targetIp_[i]->setText(jstr(owner_.cfg_.targets[i].ip));
            targetIp_[i]->setJustification(juce::Justification::centredLeft);
            addAndMakeVisible(targetIp_[i]);
            targetPort_[i] = new juce::TextEditor("tp" + juce::String(i));
            if (owner_.cfg_.targets.size() > i)
                targetPort_[i]->setText(juce::String(owner_.cfg_.targets[i].port));
            targetPort_[i]->setJustification(juce::Justification::centredLeft);
            addAndMakeVisible(targetPort_[i]);
        }
    }

    void buildRoutes() {
        routesHdr_ = new juce::Label("rh", "Routes  (surface addr -> target / slot / param)   [X clears a row]");
        routesHdr_->setFont(juce::FontOptions(12.0f, juce::Font::bold));
        addAndMakeVisible(routesHdr_);
        // Target combo items: t0..t4.
        juce::StringArray titems;
        for (int i = 0; i < ProxyConfig::kMaxTargets; ++i) titems.add("t" + juce::String(i));
        for (int i = 0; i < kMaxRoutes; ++i) {
            routeSrc_[i] = new juce::TextEditor("rs" + juce::String(i));
            if (owner_.cfg_.routes.size() > i)
                routeSrc_[i]->setText(jstr(owner_.cfg_.routes[i].srcAddr));
            routeSrc_[i]->setJustification(juce::Justification::centredLeft);
            addAndMakeVisible(routeSrc_[i]);

            routeTgt_[i] = new juce::ComboBox("rt" + juce::String(i));
            routeTgt_[i]->addItem("t0", 1);
            routeTgt_[i]->addItem("t1", 2);
            routeTgt_[i]->addItem("t2", 3);
            routeTgt_[i]->addItem("t3", 4);
            routeTgt_[i]->addItem("t4", 5);
            const int sel = owner_.cfg_.routes.size() > i ? owner_.cfg_.routes[i].targetIdx + 1 : 1;
            routeTgt_[i]->setSelectedId(juce::jlimit(1, 5, sel), juce::dontSendNotification);
            addAndMakeVisible(routeTgt_[i]);

            routeSlot_[i] = new juce::TextEditor("ro" + juce::String(i));
            if (owner_.cfg_.routes.size() > i)
                routeSlot_[i]->setText(jstr(owner_.cfg_.routes[i].slot));
            routeSlot_[i]->setJustification(juce::Justification::centredLeft);
            addAndMakeVisible(routeSlot_[i]);

            routeParam_[i] = new juce::TextEditor("rp" + juce::String(i));
            if (owner_.cfg_.routes.size() > i)
                routeParam_[i]->setText(jstr(owner_.cfg_.routes[i].param));
            routeParam_[i]->setJustification(juce::Justification::centredLeft);
            addAndMakeVisible(routeParam_[i]);

            const int row = i;
            routeClr_[i] = new juce::TextButton("rx" + juce::String(i));
            routeClr_[i]->setButtonText("x");
            routeClr_[i]->onClick = [this, row] {
                routeSrc_[row]->clear();
                routeSlot_[row]->clear();
                routeParam_[row]->clear();
                routeTgt_[row]->setSelectedId(1, juce::dontSendNotification);
            };
            addAndMakeVisible(routeClr_[i]);
        }
    }

    void buildDiscover() {
        discBtn_ = new juce::TextButton("db");
        discBtn_->setButtonText("DISCOVER");
        discBtn_->onClick = [this] {
            readInputs();  // use the currently-typed targets
            const int ti = discTgt_->getSelectedId() - 1;
            if (ti < 0 || ti >= (int) owner_.cfg_.targets.size()) {
                status_->setText("discover: pick a valid target (set ip+port)", juce::dontSendNotification);
                return;
            }
            status_->setText("discovering " + jstr(owner_.cfg_.targets[ti].ip) + "…",
                             juce::dontSendNotification);
            // Run on the message thread (this is already it); a short block is
            // acceptable for a manual button press.
            ProxyDiscovered d = owner_.engine_.discover(owner_.cfg_.targets[ti], 1500);
            if (!d.ok) {
                status_->setText("discover: no reply", juce::dontSendNotification);
                return;
            }
            std::vector<DiscoveredModel::Entry> ents;
            ents.reserve(d.params.size());
            for (const auto& p : d.params) ents.push_back({ti, p});
            discModel_.refresh(ents, discList_);
            status_->setText(juce::String(d.params.size()) + " params discovered (double-click to route)",
                             juce::dontSendNotification);
        };
        addAndMakeVisible(discBtn_);

        discTgt_ = new juce::ComboBox("dt");
        discTgt_->addItem("t0", 1);
        discTgt_->addItem("t1", 2);
        discTgt_->addItem("t2", 3);
        discTgt_->addItem("t3", 4);
        discTgt_->addItem("t4", 5);
        discTgt_->setSelectedId(1, juce::dontSendNotification);
        addAndMakeVisible(discTgt_);

        discList_ = new juce::ListBox("dl", &discModel_);
        discList_->setRowHeight(14);
        discModel_.setInsertCallback([this](int ti, const ProxyDiscovered::Param& p) {
            // Find the first empty route row and fill it.
            for (int i = 0; i < kMaxRoutes; ++i) {
                if (routeSrc_[i]->getText().trim().isEmpty() &&
                    routeSlot_[i]->getText().trim().isEmpty() &&
                    routeParam_[i]->getText().trim().isEmpty()) {
                    routeTgt_[i]->setSelectedId(ti + 1, juce::dontSendNotification);
                    routeSlot_[i]->setText(jstr(p.slot));
                    routeParam_[i]->setText(jstr(p.param));
                    status_->setText("route added — press APPLY", juce::dontSendNotification);
                    break;
                }
            }
        });
        addAndMakeVisible(discList_);

        discLbl_ = new juce::Label("dlb",
                                   "DISCOVER queries a target's slots/params via the\n"
                                   "Heddle OSC protocol (/chain/query). Double-click a\n"
                                   "line to fill the next empty route row.");
        discLbl_->setFont(juce::FontOptions(11.0f));
        addAndMakeVisible(discLbl_);
    }

    void refreshStatus() {
        const auto& cfg = owner_.cfg_;
        status_->setText(owner_.statusMsg_ + "   |   rec " + juce::String(owner_.engine_.received()) +
                            "  fwd " + juce::String(owner_.engine_.forwarded()) +
                            "  drop " + juce::String(owner_.engine_.dropped()),
                         juce::dontSendNotification);
    }

    ProxyProcessor& owner_;
    juce::Label* title_ = nullptr;
    juce::Label* status_ = nullptr;
    juce::Button* mode_ = nullptr;
    juce::Button* apply_ = nullptr;
    juce::Label* listenLbl_ = nullptr;
    juce::TextEditor* listenEd_ = nullptr;

    juce::Label* targetsHdr_ = nullptr;
    juce::ToggleButton* targetEn_[ProxyConfig::kMaxTargets] = {};
    juce::TextEditor* targetIp_[ProxyConfig::kMaxTargets] = {};
    juce::TextEditor* targetPort_[ProxyConfig::kMaxTargets] = {};

    juce::Label* routesHdr_ = nullptr;
    juce::TextEditor* routeSrc_[kMaxRoutes] = {};
    juce::ComboBox* routeTgt_[kMaxRoutes] = {};
    juce::TextEditor* routeSlot_[kMaxRoutes] = {};
    juce::TextEditor* routeParam_[kMaxRoutes] = {};
    juce::Button* routeClr_[kMaxRoutes] = {};

    juce::Button* discBtn_ = nullptr;
    juce::ComboBox* discTgt_ = nullptr;
    juce::ListBox* discList_ = nullptr;
    juce::Label* discLbl_ = nullptr;
    DiscoveredModel discModel_;
};

// Out-of-line definition: ProxyEditor is complete here.
juce::AudioProcessorEditor* ProxyProcessor::createEditor() {
    return new ProxyEditor(*this);
}

}  // namespace heddle_proxy

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return heddle_proxy::ProxyProcessor::createPluginFilter();
}
// JucePlugin_CreateInstance is provided by the generated JuceHeader.h.
