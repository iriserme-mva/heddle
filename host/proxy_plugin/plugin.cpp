// heddle-proxy (passthrough VST3) — routes one OSC surface to up to 5
// Heddle targets. Audio passes through UNCHANGED (this plugin adds no DSP);
// the OSC router (heddle_core's ProxyEngine) runs on a juce::Timer on the
// message thread, so the audio callback is never touched. See docs/PROXY.md.
//
// The editor exposes the full routing table:
//   * 3-state mode button: OFF / ONLY-CONTROL / FULL
//   * listen port (the port the surface / TouchOSC sends to)
//   * up to 5 targets (ip / port / enabled)
//   * up to 8 routes (surface addr -> target idx / slot / param), each with
//     a value mapping: arg (which float of the surface message is the value),
//     in_min / in_max (rescale to 0..1) and curve (identity/exp/invexp/scurve)
//   * DISCOVER: query a target via the Heddle OSC protocol and list its
//     slots/params; double-click a line to insert a route.
//   * RECEIVED: live list of the addresses the proxy actually got from the
//     surface (the "learn" list); double-click a line to fill a route row's
//     surface address and arg index.
//   * live counters (received / forwarded / dropped)
//
// The routing table is persisted in the plugin state (getStateInformation /
// setStateInformation) so it follows the DAW's save.
#include <cctype>
#include <cmath>
#include <string>

#include "JuceHeader.h"
#include "heddle/proxy.hpp"
#include "heddle/proxy_engine.hpp"

namespace heddle_proxy {

// The OSC router types live in heddle (the JUCE-free core); pull them into
// this namespace so the processor/editor can name them unqualified.
using namespace heddle;
using RecentMsg = heddle::ProxyEngine::RecentMsg;  // nested type, not covered by the using-directive

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
        // The ListBox RowComponent does not set a text colour before calling
        // us, so drawText would use Graphics' default (black) — invisible on
        // the dark list background. Use the palette's colours (hex inlined:
        // HeddlePalette is declared after this class in the file).
        const juce::Colour accent(0xff34c3a4);   // HeddlePalette::accent
        const juce::Colour text(0xffe9edf3);     // HeddlePalette::text
        const juce::Colour dim(0xff9aa5b4);      // HeddlePalette::textDim
        g.setColour(accent);
        g.fillRect(6, (height - 8) / 2, 3, 8);   // accent bullet
        const juce::String path = juce::String("/chain/") + jstr(e.p.slot) + "/" +
                                  jstr(e.p.param);
        g.setColour(text);
        g.setFont(juce::FontOptions("Segoe UI", 11.0f, juce::Font::plain));
        g.drawText(path, 16, 1, 2000, height, juce::Justification::left);
        g.setColour(dim);
        g.setFont(juce::FontOptions("Segoe UI", 10.0f, juce::Font::plain));
        g.drawText("t" + juce::String(e.targetIdx) + "  #" + juce::String(e.p.paramId),
                   16, 1, 2000, height, juce::Justification::right);
    }
    // JUCE 8.0.4: the double-click hook is listBoxItemDoubleClicked(row, e)
    // (there is no doubleClick(int) — that is TableListBoxModel).
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override {
        if (row < 0 || row >= (int) entries_.size() || !insert_) return;
        insert_(entries_[row].targetIdx, entries_[row].p);
    }
    // JUCE 8.0.4 ListBoxModel has no begin/endUpdate; repaint() alone does NOT
    // refresh the ListBox's cached row count, so use updateContent().
    void refresh(const std::vector<Entry>& e, juce::ListBox* list) {
        list_ = list;
        entries_ = e;
        if (list != nullptr) list->updateContent();
    }
    void setInsertCallback(InsertCb cb) { insert_ = std::move(cb); }

private:
    std::vector<Entry> entries_;
    InsertCb insert_;
    juce::ListBox* list_ = nullptr;
};

// ===========================================================================
// Received-address ("learn") list model (double-click fills a route row's
// surface address + arg index)
// ===========================================================================
class RecentModel : public juce::ListBoxModel {
public:
    struct Entry {
        std::string addr;
        int numFloats = 0;
    };
    using FillCb = std::function<void(const std::string& /*addr*/, int /*numFloats*/)>;

    int getNumRows() override { return (int) entries_.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int, int height, bool) override {
        if (row < 0 || row >= (int) entries_.size()) return;
        const auto& e = entries_[row];
        const juce::Colour accent(0xff34c3a4);
        const juce::Colour text(0xffe9edf3);
        const juce::Colour dim(0xff9aa5b4);
        g.setColour(accent);
        g.fillRect(6, (height - 8) / 2, 3, 8);
        g.setColour(text);
        g.setFont(juce::FontOptions("Segoe UI", 11.0f, juce::Font::plain));
        g.drawText(jstr(e.addr), 16, 1, 2000, height, juce::Justification::left);
        g.setColour(dim);
        g.setFont(juce::FontOptions("Segoe UI", 10.0f, juce::Font::plain));
        g.drawText(e.numFloats > 1 ? juce::String(e.numFloats) + " floats" : "1 float",
                   16, 1, 2000, height, juce::Justification::right);
    }
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override {
        if (row < 0 || row >= (int) entries_.size() || !fill_) return;
        fill_(entries_[row].addr, entries_[row].numFloats);
    }
    void refresh(const std::vector<RecentMsg>& rec, juce::ListBox* list) {
        list_ = list;
        entries_.clear();
        entries_.reserve(rec.size());
        for (const auto& m : rec) entries_.push_back({m.addr, m.numFloats});
        if (list != nullptr) list->updateContent();
    }
    void setFillCallback(FillCb cb) { fill_ = std::move(cb); }

private:
    std::vector<Entry> entries_;
    FillCb fill_;
    juce::ListBox* list_ = nullptr;
};

// ===========================================================================
// Editor
// ===========================================================================
// Shared dark palette for the proxy editor. Every child gets its colours set
// explicitly so the panel looks the same under the default host LAF, the
// headless capture harness, or any third-party VST host.
struct HeddlePalette {
    static juce::Colour base()         { return juce::Colour(0xff171b21); }
    static juce::Colour baseTop()      { return juce::Colour(0xff1f252e); }
    static juce::Colour panel()        { return juce::Colour(0xff232a35); }
    static juce::Colour panelFocus()   { return juce::Colour(0xff2c3644); }
    static juce::Colour accent()       { return juce::Colour(0xff34c3a4); }
    static juce::Colour accentHover()  { return juce::Colour(0xff46d6b7); }
    static juce::Colour accentDown()   { return juce::Colour(0xff27a98b); }
    static juce::Colour neutral()      { return juce::Colour(0xff2c3441); }
    static juce::Colour neutralHover() { return juce::Colour(0xff3a4453); }
    static juce::Colour neutralDown()  { return juce::Colour(0xff1f252e); }
    static juce::Colour text()         { return juce::Colour(0xffe9edf3); }
    static juce::Colour textDim()      { return juce::Colour(0xff9aa5b4); }
    static juce::Colour textFaint()    { return juce::Colour(0xff657082); }
    static juce::Colour divider()      { return juce::Colour(0xff2b333f); }
    static juce::Colour listBg()       { return juce::Colour(0xff1a1f27); }
    static juce::Colour listSel()      { return juce::Colour(0xff2c4a44); }
};

// Small UI font: Segoe UI where available (it is, on Windows), falling back to
// JUCE's default typeface otherwise.
static juce::Font hf(float size, int style = juce::Font::plain) {
    return juce::FontOptions("Segoe UI", size, style);
}

// Rounded, flat action button. Accent kind = the primary call-to-action (teal
// fill, dark label); Neutral kind = secondary controls (slate fill, light
// label). Derives from juce::Button so triggerClick()/onClick work exactly
// like the stock TextButton it replaces.
class HeddleButton : public juce::Button {
public:
    enum class Kind { Accent, Neutral };
    HeddleButton(const juce::String& nm, const juce::String& txt, Kind kind)
        : juce::Button(nm), kind_(kind) {
        setButtonText(txt);
    }
    void paintButton(juce::Graphics& g, bool over, bool down) override {
        const bool accent = kind_ == Kind::Accent;
        const juce::Colour base  = accent ? HeddlePalette::accent()      : HeddlePalette::neutral();
        const juce::Colour hover = accent ? HeddlePalette::accentHover() : HeddlePalette::neutralHover();
        const juce::Colour dn    = accent ? HeddlePalette::accentDown()  : HeddlePalette::neutralDown();
        const juce::Colour label = accent ? HeddlePalette::base()        : HeddlePalette::text();
        const juce::Colour fill  = down ? dn : (over ? hover : base);
        g.setColour(fill);
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 5.0f);
        g.setColour(label);
        g.setFont(hf(12.0f, juce::Font::bold));
        g.drawText(getButtonText(), getLocalBounds().reduced(4, 0),
                   juce::Justification::centred);
    }
    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override  { repaint(); }

private:
    Kind kind_;
};

class ProxyEditor : public juce::AudioProcessorEditor,
                    public juce::Timer {
public:
    explicit ProxyEditor(ProxyProcessor& owner)
        : juce::AudioProcessorEditor(owner), owner_(owner) {
        setSize(860, 700);
        toBack();

        title_ = new juce::Label("t", "Heddle Proxy - OSC fan-out");
        title_->setFont(hf(15.0f, juce::Font::bold));
        title_->setColour(juce::Label::textColourId, HeddlePalette::text());
        addAndMakeVisible(title_);

        status_ = new juce::Label("s", owner_.statusMsg_.toRawUTF8());
        status_->setFont(hf(11.0f));
        status_->setJustificationType(juce::Justification::centredRight);
        status_->setColour(juce::Label::textColourId, HeddlePalette::textFaint());
        addAndMakeVisible(status_);

        // Mode 3-state button (OFF / ONLY-CONTROL / FULL).
        mode_ = new HeddleButton("mode", "MODE: " + modeLabel(owner_.cfg_.mode),
                                 HeddleButton::Kind::Neutral);
        mode_->onClick = [this] {
            cycleMode();
            owner_.commitConfig("mode -> " + modeLabel(owner_.cfg_.mode).toStdString());
            mode_->setButtonText("MODE: " + modeLabel(owner_.cfg_.mode));
        };
        addAndMakeVisible(mode_);

        apply_ = new HeddleButton("apply", "APPLY", HeddleButton::Kind::Accent);
        apply_->onClick = [this] {
            readInputs();
            owner_.commitConfig("applied");
            refreshStatus();
        };
        addAndMakeVisible(apply_);

        listenLbl_ = new juce::Label("ll", "LISTEN PORT");
        styleFaintLabel(listenLbl_);
        addAndMakeVisible(listenLbl_);
        listenEd_ = new juce::TextEditor("lpe");
        listenEd_->setText(juce::String(owner_.cfg_.listenPort));
        listenEd_->setJustification(juce::Justification::centredLeft);
        styleInput(listenEd_);
        addAndMakeVisible(listenEd_);

        buildTargets();
        buildRoutes();
        buildDiscover();
        buildLearn();

        // Poll the live counters + status.
        startTimerHz(4);
        refreshStatus();
        resized();  // layout now that all children exist (first setSize skipped it)
    }

    ~ProxyEditor() override = default;

    void timerCallback() override {
        refreshStatus();
        refreshLearn();
    }

    void paint(juce::Graphics& g) override {
        // Base background: flat base colour, with a subtle top gradient from
        // baseTop to base over the first 90 px (drawn as 1px interpolated lines,
        // since this JUCE build's Graphics has no brush/gradient fill API).
        g.setColour(HeddlePalette::base());
        g.fillRect(0, 0, getWidth(), getHeight());
        const int gh = 90;
        const juce::Colour top = HeddlePalette::baseTop();
        const juce::Colour bot = HeddlePalette::base();
        for (int y = 0; y < gh; ++y) {
            const float t = (float)y / (float)gh;
            const juce::uint8 r  = (juce::uint8)(top.getRed()   + t * (bot.getRed()   - top.getRed()));
            const juce::uint8 gr = (juce::uint8)(top.getGreen() + t * (bot.getGreen() - top.getGreen()));
            const juce::uint8 b  = (juce::uint8)(top.getBlue()  + t * (bot.getBlue()  - top.getBlue()));
            g.setColour(juce::Colour::fromRGB(r, gr, b));
            g.fillRect(0, y, getWidth(), 1);
        }

        // Title bar band with an accent underline.
        g.setColour(HeddlePalette::baseTop());
        g.fillRect(0, 0, getWidth(), 52);
        g.setColour(HeddlePalette::accent());
        g.fillRoundedRectangle(16, 30, 44, 4, 2.0f);

        // Section dividers (behind the row groups). Offsets must match resized():
        // each section starts at base + 36 (header) + rows*rowH + 10.
        g.setColour(HeddlePalette::divider());
        g.fillRect(16, 60, getWidth() - 32, 1);
        const int t0 = 78, tRowH = 30;
        const int r0 = t0 + 36 + ProxyConfig::kMaxTargets * tRowH + 10;
        g.fillRect(16, r0 - 12, getWidth() - 32, 1);
        const int d0 = r0 + 36 + kMaxRoutes * 36 + 10;
        g.fillRect(16, d0 - 12, getWidth() - 32, 1);
        g.fillRect(668, d0 + 8, 1, 88);  // route table | learn panel
    }

    void resized() override {
        // Guard: the ctor's first setSize() fires resized() before the child
        // pointers below exist (headless hosts never opened the editor, so this
        // stayed latent).
        if (title_ == nullptr || targetsHdr_ == nullptr ||
            routesHdr_ == nullptr || discLbl_ == nullptr)
            return;
        title_->setBounds(74, 14, 360, 22);
        status_->setBounds(getWidth() - 332, 16, 316, 18);

        mode_->setBounds(16, 62, 170, 30);
        apply_->setBounds(getWidth() - 106, 62, 90, 30);
        listenLbl_->setBounds(200, 68, 96, 20);
        listenEd_->setBounds(300, 66, 74, 24);

        // Targets block
        const int t0 = 78;
        targetsHdr_->setBounds(16, t0, 360, 16);
        tgtColEn_->setBounds(16, t0 + 18, 60, 14);
        tgtColIp_->setBounds(86, t0 + 18, 150, 14);
        tgtColPt_->setBounds(242, t0 + 18, 64, 14);
        const int tRowH = 30;
        for (int i = 0; i < ProxyConfig::kMaxTargets; ++i) {
            const int y = t0 + 36 + i * tRowH;
            targetEn_[i]->setBounds(28, y + 4, 40, 20);
            targetIp_[i]->setBounds(86, y, 150, 24);
            targetPort_[i]->setBounds(242, y, 64, 24);
        }

        // Routes block (row height 36: value/mapping controls under the addr row)
        const int r0 = t0 + 36 + ProxyConfig::kMaxTargets * tRowH + 10;
        routesHdr_->setBounds(16, r0, 480, 16);
        rtColSrc_->setBounds(16, r0 + 18, 150, 14);
        rtColTgt_->setBounds(176, r0 + 18, 56, 14);
        rtColSlot_->setBounds(242, r0 + 18, 120, 14);
        rtColPar_->setBounds(372, r0 + 18, 140, 14);
        rtColArg_->setBounds(528, r0 + 18, 40, 14);
        rtColMM_->setBounds(572, r0 + 18, 70, 14);
        rtColCv_->setBounds(648, r0 + 18, 56, 14);
        const int rRowH = 36;
        const int xSrc = 16, xTgt = 176, xSlot = 242, xParam = 372;
        const int xArg = 528, xMin = 572, xMax = 606, xCurve = 648;
        for (int i = 0; i < kMaxRoutes; ++i) {
            const int y = r0 + 36 + i * rRowH;
            routeSrc_[i]->setBounds(xSrc, y, 150, 24);
            routeTgt_[i]->setBounds(xTgt, y, 56, 26);
            routeSlot_[i]->setBounds(xSlot, y, 120, 24);
            routeParam_[i]->setBounds(xParam, y, 140, 24);
            routeArg_[i]->setBounds(xArg, y, 36, 24);
            routeMin_[i]->setBounds(xMin, y, 30, 24);
            routeMax_[i]->setBounds(xMax, y, 38, 24);
            routeCurve_[i]->setBounds(xCurve, y, 56, 26);
            routeClr_[i]->setBounds(708, y, 48, 26);
        }

        // Learn (received addresses) panel — right of the route table.
        const int d0 = r0 + 36 + kMaxRoutes * rRowH + 10;
        learnHdr_->setBounds(688, d0 + 8, 160, 16);
        learnList_->setBounds(688, d0 + 28, 156, 68);

        // Discover block
        discHdr_->setBounds(16, d0 + 8, 400, 16);
        discBtn_->setBounds(16, d0 + 32, 120, 30);
        discTgt_->setBounds(152, d0 + 32, 104, 28);
        discList_->setBounds(16, d0 + 70, 300, 70);
        discLbl_->setBounds(332, d0 + 70, 320, 70);
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

    // ---- Value-mapping field helpers --------------------------------------
    // The mapping inputs (arg / in_min / in_max) are free text; an empty or
    // invalid field falls back to the route default so a half-typed row never
    // breaks APPLY. Curves are a closed set, so the combo always yields one.
    static int parseArg(const juce::String& s, int def) {
        const juce::String t = s.trim();
        if (t.isEmpty()) return def;
        if (t[0] == '-' || t[0] == '+') return def;  // negative arg is invalid
        for (int i = 0; i < t.length(); ++i)
            if (!std::isdigit((unsigned char) t[i])) return def;
        return t.getIntValue();
    }
    static double parseDbl(const juce::String& s, double def) {
        const juce::String t = s.trim();
        if (t.isEmpty()) return def;
        bool ok = false;
        for (int i = 0; i < t.length(); ++i) {
            const char c = t[i];
            if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9') ||
                c == 'e' || c == 'E') { ok = true; }
            else return def;
        }
        return ok ? (double) t.getDoubleValue() : def;
    }
    // 0 -> "0", 0.5 -> "0.5", 1 -> "1" (3-decimal format, trailing zeros trimmed).
    static juce::String fmtDbl(double v) {
        juce::String s = juce::String(v, 3);
        while (s.endsWith("0")) s = s.dropLastCharacters(1);
        if (s.endsWith(".")) s = s.dropLastCharacters(1);
        return s;
    }
    // The curve combo offers identity/exp/invexp/scurve (ids 1..4); the enum
    // has an extra `linear` alias at index 1, so the id<->enum mapping is
    // explicit rather than an offset.
    static ProxyCurve curveFromId(int id) {
        switch (id) {
            case 2: return ProxyCurve::exp;
            case 3: return ProxyCurve::invexp;
            case 4: return ProxyCurve::scurve;
            case 1:
            default: return ProxyCurve::identity;
        }
    }
    static int idFromCurve(ProxyCurve c) {
        switch (c) {
            case ProxyCurve::exp: return 2;
            case ProxyCurve::invexp: return 3;
            case ProxyCurve::scurve: return 4;
            case ProxyCurve::linear:
            case ProxyCurve::identity:
            default: return 1;
        }
    }

    // ---- Shared control styling -------------------------------------------
    static void styleFaintLabel(juce::Label* l) {
        l->setFont(hf(9.0f, juce::Font::bold));
        l->setColour(juce::Label::textColourId, HeddlePalette::textFaint());
    }
    static void styleInput(juce::TextEditor* e) {
        e->setFont(hf(12.0f));
        e->setColour(juce::TextEditor::textColourId, HeddlePalette::text());
        e->setColour(juce::TextEditor::backgroundColourId, HeddlePalette::panel());
        e->setColour(juce::TextEditor::highlightColourId, HeddlePalette::panelFocus());
    }
    static void styleCombo(juce::ComboBox* c) {
        c->setColour(juce::ComboBox::textColourId, HeddlePalette::text());
        c->setColour(juce::ComboBox::outlineColourId, HeddlePalette::panelFocus());
        c->setColour(juce::ComboBox::backgroundColourId, HeddlePalette::panel());
        c->setColour(juce::ComboBox::buttonColourId, HeddlePalette::panelFocus());
        c->setColour(juce::ComboBox::arrowColourId, HeddlePalette::textDim());
        c->setColour(juce::ComboBox::focusedOutlineColourId, HeddlePalette::accent());
    }
    static void styleList(juce::ListBox* l) {
        l->setColour(juce::ListBox::backgroundColourId, HeddlePalette::listBg());
        l->setColour(juce::ListBox::outlineColourId, HeddlePalette::panelFocus());
        l->setColour(juce::ListBox::textColourId, HeddlePalette::text());
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
            // Value mapping (empty/invalid field -> route default).
            r.arg = parseArg(routeArg_[i]->getText(), 0);
            r.inMin = parseDbl(routeMin_[i]->getText(), 0.0);
            r.inMax = parseDbl(routeMax_[i]->getText(), 1.0);
            r.curve = curveFromId(routeCurve_[i]->getSelectedId());
            if (!r.slot.empty() && !r.param.empty())
                rs.push_back(std::move(r));
        }
        owner_.cfg_.routes = std::move(rs);
        (void) err;
    }

    void buildTargets() {
        targetsHdr_ = new juce::Label("th", "TARGETS");
        styleFaintLabel(targetsHdr_);
        addAndMakeVisible(targetsHdr_);
        tgtColEn_ = new juce::Label("tce", "ON");
        styleFaintLabel(tgtColEn_);
        addAndMakeVisible(tgtColEn_);
        tgtColIp_ = new juce::Label("tci", "HOST / IP");
        styleFaintLabel(tgtColIp_);
        addAndMakeVisible(tgtColIp_);
        tgtColPt_ = new juce::Label("tcp", "PORT");
        styleFaintLabel(tgtColPt_);
        addAndMakeVisible(tgtColPt_);
        for (int i = 0; i < ProxyConfig::kMaxTargets; ++i) {
            targetEn_[i] = new juce::ToggleButton("te" + juce::String(i));
            targetEn_[i]->setButtonText("ON");
            targetEn_[i]->setToggleState(owner_.cfg_.targets.size() > i ? owner_.cfg_.targets[i].enabled : true,
                                         juce::dontSendNotification);
            targetEn_[i]->setClickingTogglesState(true);
            targetEn_[i]->setColour(juce::ToggleButton::textColourId, HeddlePalette::text());
            targetEn_[i]->setColour(juce::ToggleButton::tickColourId, HeddlePalette::accent());
            addAndMakeVisible(targetEn_[i]);
            targetIp_[i] = new juce::TextEditor("ti" + juce::String(i));
            if (owner_.cfg_.targets.size() > i)
                targetIp_[i]->setText(jstr(owner_.cfg_.targets[i].ip));
            targetIp_[i]->setJustification(juce::Justification::centredLeft);
            styleInput(targetIp_[i]);
            addAndMakeVisible(targetIp_[i]);
            targetPort_[i] = new juce::TextEditor("tp" + juce::String(i));
            if (owner_.cfg_.targets.size() > i)
                targetPort_[i]->setText(juce::String(owner_.cfg_.targets[i].port));
            targetPort_[i]->setJustification(juce::Justification::centredLeft);
            styleInput(targetPort_[i]);
            addAndMakeVisible(targetPort_[i]);
        }
    }

    void buildRoutes() {
        routesHdr_ = new juce::Label("rh", "ROUTES   (surface address -> target / slot / param)");
        styleFaintLabel(routesHdr_);
        addAndMakeVisible(routesHdr_);
        rtColSrc_ = new juce::Label("rcs", "SURFACE ADDR");
        styleFaintLabel(rtColSrc_);
        addAndMakeVisible(rtColSrc_);
        rtColTgt_ = new juce::Label("rct", "TGT");
        styleFaintLabel(rtColTgt_);
        addAndMakeVisible(rtColTgt_);
        rtColSlot_ = new juce::Label("rcsl", "SLOT");
        styleFaintLabel(rtColSlot_);
        addAndMakeVisible(rtColSlot_);
        rtColPar_ = new juce::Label("rcp", "PARAM");
        styleFaintLabel(rtColPar_);
        addAndMakeVisible(rtColPar_);
        rtColArg_ = new juce::Label("rca", "ARG");
        styleFaintLabel(rtColArg_);
        addAndMakeVisible(rtColArg_);
        rtColMM_ = new juce::Label("rcmm", "MIN  MAX");
        styleFaintLabel(rtColMM_);
        addAndMakeVisible(rtColMM_);
        rtColCv_ = new juce::Label("rcc", "CURVE");
        styleFaintLabel(rtColCv_);
        addAndMakeVisible(rtColCv_);
        // Target combo items: t0..t4.
        juce::StringArray titems;
        for (int i = 0; i < ProxyConfig::kMaxTargets; ++i) titems.add("t" + juce::String(i));
        for (int i = 0; i < kMaxRoutes; ++i) {
            routeSrc_[i] = new juce::TextEditor("rs" + juce::String(i));
            if (owner_.cfg_.routes.size() > i)
                routeSrc_[i]->setText(jstr(owner_.cfg_.routes[i].srcAddr));
            routeSrc_[i]->setJustification(juce::Justification::centredLeft);
            styleInput(routeSrc_[i]);
            addAndMakeVisible(routeSrc_[i]);

            routeTgt_[i] = new juce::ComboBox("rt" + juce::String(i));
            routeTgt_[i]->addItem("t0", 1);
            routeTgt_[i]->addItem("t1", 2);
            routeTgt_[i]->addItem("t2", 3);
            routeTgt_[i]->addItem("t3", 4);
            routeTgt_[i]->addItem("t4", 5);
            const int sel = owner_.cfg_.routes.size() > i ? owner_.cfg_.routes[i].targetIdx + 1 : 1;
            routeTgt_[i]->setSelectedId(juce::jlimit(1, 5, sel), juce::dontSendNotification);
            styleCombo(routeTgt_[i]);
            addAndMakeVisible(routeTgt_[i]);

            routeSlot_[i] = new juce::TextEditor("ro" + juce::String(i));
            if (owner_.cfg_.routes.size() > i)
                routeSlot_[i]->setText(jstr(owner_.cfg_.routes[i].slot));
            routeSlot_[i]->setJustification(juce::Justification::centredLeft);
            styleInput(routeSlot_[i]);
            addAndMakeVisible(routeSlot_[i]);

            routeParam_[i] = new juce::TextEditor("rp" + juce::String(i));
            if (owner_.cfg_.routes.size() > i)
                routeParam_[i]->setText(jstr(owner_.cfg_.routes[i].param));
            routeParam_[i]->setJustification(juce::Justification::centredLeft);
            styleInput(routeParam_[i]);
            addAndMakeVisible(routeParam_[i]);

            // Value mapping for this route (ARG / MIN / MAX / CURVE).
            const ProxyRoute* rt = owner_.cfg_.routes.size() > i ? &owner_.cfg_.routes[i] : nullptr;
            routeArg_[i] = new juce::TextEditor("ra" + juce::String(i));
            routeArg_[i]->setText(rt ? juce::String(rt->arg) : juce::String("0"));
            routeArg_[i]->setJustification(juce::Justification::centred);
            styleInput(routeArg_[i]);
            addAndMakeVisible(routeArg_[i]);

            routeMin_[i] = new juce::TextEditor("rm" + juce::String(i));
            routeMin_[i]->setText(rt ? fmtDbl(rt->inMin) : juce::String("0"));
            routeMin_[i]->setJustification(juce::Justification::centred);
            styleInput(routeMin_[i]);
            addAndMakeVisible(routeMin_[i]);

            routeMax_[i] = new juce::TextEditor("rxm" + juce::String(i));
            routeMax_[i]->setText(rt ? fmtDbl(rt->inMax) : juce::String("1"));
            routeMax_[i]->setJustification(juce::Justification::centred);
            styleInput(routeMax_[i]);
            addAndMakeVisible(routeMax_[i]);

            routeCurve_[i] = new juce::ComboBox("rc" + juce::String(i));
            static const char* kCurveNames[] = {"identity", "exp", "invexp", "scurve"};
            for (int c = 0; c < 4; ++c) routeCurve_[i]->addItem(kCurveNames[c], c + 1);
            const int cSel = rt ? idFromCurve(rt->curve) : 1;
            routeCurve_[i]->setSelectedId(juce::jlimit(1, 4, cSel), juce::dontSendNotification);
            styleCombo(routeCurve_[i]);
            addAndMakeVisible(routeCurve_[i]);

            const int row = i;
            routeClr_[i] = new juce::TextButton("rx" + juce::String(i));
            routeClr_[i]->setButtonText("x");
            routeClr_[i]->setColour(juce::TextButton::buttonColourId, HeddlePalette::neutral());
            routeClr_[i]->setColour(juce::TextButton::textColourOffId, HeddlePalette::textDim());
            routeClr_[i]->setColour(juce::TextButton::textColourOnId, HeddlePalette::accent());
            routeClr_[i]->onClick = [this, row] {
                routeSrc_[row]->clear();
                routeSlot_[row]->clear();
                routeParam_[row]->clear();
                routeTgt_[row]->setSelectedId(1, juce::dontSendNotification);
                routeArg_[row]->setText(juce::String("0"));
                routeMin_[row]->setText(juce::String("0"));
                routeMax_[row]->setText(juce::String("1"));
                routeCurve_[row]->setSelectedId(1, juce::dontSendNotification);
            };
            addAndMakeVisible(routeClr_[i]);
        }
    }

    void buildDiscover() {
        discHdr_ = new juce::Label("dh", "DISCOVERED PARAMS");
        styleFaintLabel(discHdr_);
        addAndMakeVisible(discHdr_);

        discBtn_ = new HeddleButton("db", "DISCOVER", HeddleButton::Kind::Accent);
        discBtn_->onClick = [this] {
            readInputs();  // use the currently-typed targets
            const int ti = discTgt_->getSelectedId() - 1;
            if (ti < 0 || ti >= (int) owner_.cfg_.targets.size()) {
                status_->setText("discover: pick a valid target (set ip+port)", juce::dontSendNotification);
                return;
            }
            status_->setText("discovering " + jstr(owner_.cfg_.targets[ti].ip) + "...",
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
        styleCombo(discTgt_);
        addAndMakeVisible(discTgt_);

        discList_ = new juce::ListBox("dl", &discModel_);
        discList_->setRowHeight(16);
        styleList(discList_);
        discModel_.setInsertCallback([this](int ti, const ProxyDiscovered::Param& p) {
            // Find the first empty route row and fill it.
            for (int i = 0; i < kMaxRoutes; ++i) {
                if (routeSrc_[i]->getText().trim().isEmpty() &&
                    routeSlot_[i]->getText().trim().isEmpty() &&
                    routeParam_[i]->getText().trim().isEmpty()) {
                    routeTgt_[i]->setSelectedId(ti + 1, juce::dontSendNotification);
                    routeSlot_[i]->setText(jstr(p.slot));
                    routeParam_[i]->setText(jstr(p.param));
                    status_->setText("route added - press APPLY", juce::dontSendNotification);
                    break;
                }
            }
        });
        addAndMakeVisible(discList_);

        discLbl_ = new juce::Label("dlb",
                                   "DISCOVER queries a target's slots / params over the\n"
                                   "Heddle OSC protocol (/chain/query). Double-click a line\n"
                                   "to fill the next empty route row, then press APPLY.");
        discLbl_->setFont(hf(10.5f));
        discLbl_->setColour(juce::Label::textColourId, HeddlePalette::textFaint());
        addAndMakeVisible(discLbl_);
    }

    // ---- Learn panel -------------------------------------------------------
    // RECEIVED lists the OSC addresses the proxy has actually seen from the
    // surface (newest first, deduped) — so a user never has to guess what
    // TouchOSC sends. Double-click a line to fill the next empty route row's
    // SURFACE ADDR and ARG (arg = 1 when the message carries several floats,
    // so a multi-value control is routed on its second value — adjust if the
    // first float is the value you want).
    void buildLearn() {
        learnHdr_ = new juce::Label("lh", "RECEIVED (double-click to use)");
        styleFaintLabel(learnHdr_);
        addAndMakeVisible(learnHdr_);

        learnList_ = new juce::ListBox("rl", &learnModel_);
        learnList_->setRowHeight(16);
        styleList(learnList_);
        learnModel_.setFillCallback([this](const std::string& addr, int numFloats) {
            int row = -1;
            for (int i = 0; i < kMaxRoutes; ++i) {
                if (routeSrc_[i]->getText().trim().isEmpty() &&
                    routeSlot_[i]->getText().trim().isEmpty() &&
                    routeParam_[i]->getText().trim().isEmpty()) { row = i; break; }
            }
            if (row < 0) {  // table full: reuse the first row (keep its mapping)
                row = 0;
                routeSlot_[row]->clear();
                routeParam_[row]->clear();
            }
            routeSrc_[row]->setText(jstr(addr));
            routeArg_[row]->setText(juce::String(numFloats > 1 ? 1 : 0));
            status_->setText("route filled from RECEIVED - press APPLY",
                             juce::dontSendNotification);
        });
        addAndMakeVisible(learnList_);
    }

    void refreshStatus() {
        const auto& cfg = owner_.cfg_;
        status_->setText(owner_.statusMsg_ + "   |   rec " + juce::String(owner_.engine_.received()) +
                            "  fwd " + juce::String(owner_.engine_.forwarded()) +
                            "  drop " + juce::String(owner_.engine_.dropped()),
                         juce::dontSendNotification);
    }

    // Poll the engine's received-address list and refresh the learn panel only
    // when it changed (the list is bounded and deduped, so comparison is cheap
    // and keeps the ListBox from redrawing every 250 ms tick).
    void refreshLearn() {
        const std::vector<RecentMsg> rec = owner_.engine_.recent();
        std::string sig;
        sig.reserve(rec.size() * 28);
        for (const auto& m : rec) {
            sig += m.addr;
            sig += "|";
            sig += std::to_string(m.numFloats);
            sig += ";";
        }
        if (sig == lastLearnSig_) return;
        lastLearnSig_ = std::move(sig);
        learnModel_.refresh(rec, learnList_);
    }

    ProxyProcessor& owner_;
    juce::Label* title_ = nullptr;
    juce::Label* status_ = nullptr;
    juce::Button* mode_ = nullptr;
    juce::Button* apply_ = nullptr;
    juce::Label* listenLbl_ = nullptr;
    juce::TextEditor* listenEd_ = nullptr;

    juce::Label* targetsHdr_ = nullptr;
    juce::Label* tgtColEn_ = nullptr;
    juce::Label* tgtColIp_ = nullptr;
    juce::Label* tgtColPt_ = nullptr;
    juce::ToggleButton* targetEn_[ProxyConfig::kMaxTargets] = {};
    juce::TextEditor* targetIp_[ProxyConfig::kMaxTargets] = {};
    juce::TextEditor* targetPort_[ProxyConfig::kMaxTargets] = {};

    juce::Label* routesHdr_ = nullptr;
    juce::Label* rtColSrc_ = nullptr;
    juce::Label* rtColTgt_ = nullptr;
    juce::Label* rtColSlot_ = nullptr;
    juce::Label* rtColPar_ = nullptr;
    juce::Label* rtColArg_ = nullptr;
    juce::Label* rtColMM_ = nullptr;
    juce::Label* rtColCv_ = nullptr;
    juce::TextEditor* routeSrc_[kMaxRoutes] = {};
    juce::ComboBox* routeTgt_[kMaxRoutes] = {};
    juce::TextEditor* routeSlot_[kMaxRoutes] = {};
    juce::TextEditor* routeParam_[kMaxRoutes] = {};
    juce::TextEditor* routeArg_[kMaxRoutes] = {};
    juce::TextEditor* routeMin_[kMaxRoutes] = {};
    juce::TextEditor* routeMax_[kMaxRoutes] = {};
    juce::ComboBox* routeCurve_[kMaxRoutes] = {};
    juce::Button* routeClr_[kMaxRoutes] = {};

    juce::Button* discBtn_ = nullptr;
    juce::Label* discHdr_ = nullptr;
    juce::ComboBox* discTgt_ = nullptr;
    juce::ListBox* discList_ = nullptr;
    juce::Label* discLbl_ = nullptr;
    DiscoveredModel discModel_;

    juce::Label* learnHdr_ = nullptr;
    juce::ListBox* learnList_ = nullptr;
    RecentModel learnModel_;
    std::string lastLearnSig_;
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
