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
//     slots/params; drag a line onto a route row to fill it.
//   * RECEIVED: live list of the addresses the proxy actually got from the
//     surface (the "learn" list); drag a line onto a route row to fill its
//     surface address and arg index.
//   * live counters (received / forwarded / dropped)
//
// The routing table is persisted in the plugin state (getStateInformation /
// setStateInformation) so it follows the DAW's save.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
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
        // Default (no state) config: listen on 9100 on ALL interfaces (a
        // TouchOSC surface usually sits on the LAN, not loopback),
        // only-control, empty table. The engine is started lazily from the
        // message thread (see timerCallback) so a throwaway instance created
        // during VST3 factory enumeration never binds a socket or touches
        // the audio thread.
        cfg_.listenPort = 9100;
        cfg_.listenHost = "0.0.0.0";
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
// Drag-and-drop payload types.
//
// The editor is a DragAndDropContainer (the whole window), and the two
// source lists — DISCOVERED PARAMS and RECEIVED — are ListBoxes, which JUCE
// turns into drag sources when the model overrides
// ListBoxModel::getDragSourceDescription (JUCE 8.0.4, juce_ListBox.cpp
// mouseDrag -> startDragging). The payload is a var with a `type` property:
//   type = "discovered"  : targetIdx, slot, param (a /chain/slot/param line)
//   type = "received"    : addr, numFloats (an address the proxy got live)
//
// Drop targets are the per-cell highlight zones drawn by RouteTableZone
// (below) — the user drags ANY line onto ANY route row and sets SURFACE /
// SLOT / PARAM (and TGT / ARG) from one gesture. No double-click anywhere.
// ===========================================================================
inline juce::var dragVarDiscovered(int targetIdx, const std::string& slot,
                                   const std::string& param) {
    auto obj = std::make_shared<juce::DynamicObject>();
    obj->setProperty("type", "discovered");
    obj->setProperty("targetIdx", targetIdx);
    obj->setProperty("slot", juce::String::fromUTF8(slot.data()));
    obj->setProperty("param", juce::String::fromUTF8(param.data()));
    // var(ReferenceCountedObject*) ref-counts the object; the shared_ptr
    // below still owns one reference until it goes out of scope.
    return juce::var(obj.get());
}
inline juce::var dragVarReceived(const std::string& addr, int numFloats) {
    auto obj = std::make_shared<juce::DynamicObject>();
    obj->setProperty("type", "received");
    obj->setProperty("addr", juce::String::fromUTF8(addr.data()));
    obj->setProperty("numFloats", numFloats);
    return juce::var(obj.get());
}
inline bool isDragDescription(const juce::var& d) {
    const juce::var t = d.getProperty("type", juce::var());
    return t.isString() && (t.toString() == "discovered" ||
                            t.toString() == "received");
}

// ===========================================================================
// Discovered-params list model (drag a line onto a route row)
// ===========================================================================
class DiscoveredModel : public juce::ListBoxModel {
public:
    struct Entry {
        int targetIdx = 0;
        ProxyDiscovered::Param p;
    };

    int getNumRows() override { return (int) entries_.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int, int height, bool selected) override {
        if (row < 0 || row >= (int) entries_.size()) return;
        const auto& e = entries_[row];
        // The ListBox RowComponent does not set a text colour before calling
        // us, so drawText would use Graphics' default (black) — invisible on
        // the dark list background. Use the palette's colours (hex inlined:
        // HeddlePalette is declared after this class in the file).
        const juce::Colour accent(0xff34c3a4);   // HeddlePalette::accent
        const juce::Colour text(0xffe9edf3);     // HeddlePalette::text
        const juce::Colour dim(0xffaeb9c8);      // HeddlePalette::textDim
        if (selected) {
            g.setColour(juce::Colour(0xff2c4a44));
            g.fillRect(0, 0, 2000, height);
        }
        g.setColour(accent);
        g.fillRect(6, (height - 8) / 2, 3, 8);   // accent bullet
        const juce::String path = juce::String("/chain/") + jstr(e.p.slot) + "/" +
                                  jstr(e.p.param);
        g.setColour(text);
        g.setFont(juce::FontOptions("Segoe UI", 12.0f, juce::Font::plain));
        g.drawText(path, 16, 1, 2000, height, juce::Justification::left);
        g.setColour(dim);
        g.setFont(juce::FontOptions("Segoe UI", 11.0f, juce::Font::plain));
        g.drawText("t" + juce::String(e.targetIdx) + "  #" + juce::String(e.p.paramId)
                       + "   (drag onto a route row)",
                   16, 1, 2000, height, juce::Justification::right);
    }
    // Row -> drag payload. The ListBox drags the row on mouseDrag; the
    // description carries everything the drop target needs.
    juce::var getDragSourceDescription(
        const juce::SparseSet<int>& rows) override {
        if (rows.size() <= 0) return juce::var();
        const int row = rows[0];
        if (row < 0 || row >= (int) entries_.size()) return juce::var();
        const auto& e = entries_[row];
        return dragVarDiscovered(e.targetIdx, e.p.slot, e.p.param);
    }
    // JUCE 8.0.4 ListBoxModel has no begin/endUpdate; repaint() alone does NOT
    // refresh the ListBox's cached row count, so use updateContent().
    void refresh(const std::vector<Entry>& e, juce::ListBox* list) {
        list_ = list;
        entries_ = e;
        if (list != nullptr) list->updateContent();
    }

private:
    std::vector<Entry> entries_;
    juce::ListBox* list_ = nullptr;
};

// ===========================================================================
// Received-address ("learn") list model (drag a line onto a route row)
// ===========================================================================
class RecentModel : public juce::ListBoxModel {
public:
    struct Entry {
        std::string addr;
        int numFloats = 0;
    };

    int getNumRows() override { return (int) entries_.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int, int height, bool selected) override {
        if (row < 0 || row >= (int) entries_.size()) return;
        const auto& e = entries_[row];
        const juce::Colour accent(0xff34c3a4);
        const juce::Colour text(0xffe9edf3);
        const juce::Colour dim(0xffaeb9c8);
        if (selected) {
            g.setColour(juce::Colour(0xff2c4a44));
            g.fillRect(0, 0, 2000, height);
        }
        g.setColour(accent);
        g.fillRect(6, (height - 8) / 2, 3, 8);
        g.setColour(text);
        g.setFont(juce::FontOptions("Segoe UI", 12.0f, juce::Font::plain));
        g.drawText(jstr(e.addr), 16, 1, 2000, height, juce::Justification::left);
        g.setColour(dim);
        g.setFont(juce::FontOptions("Segoe UI", 11.0f, juce::Font::plain));
        g.drawText((e.numFloats > 1 ? juce::String(e.numFloats) + " floats" : "1 float")
                       + "   (drag onto a route row)",
                   16, 1, 2000, height, juce::Justification::right);
    }
    juce::var getDragSourceDescription(
        const juce::SparseSet<int>& rows) override {
        if (rows.size() <= 0) return juce::var();
        const int row = rows[0];
        if (row < 0 || row >= (int) entries_.size()) return juce::var();
        const auto& e = entries_[row];
        return dragVarReceived(e.addr, e.numFloats);
    }
    void refresh(const std::vector<RecentMsg>& rec, juce::ListBox* list) {
        list_ = list;
        entries_.clear();
        entries_.reserve(rec.size());
        for (const auto& m : rec) entries_.push_back({m.addr, m.numFloats});
        if (list != nullptr) list->updateContent();
    }

private:
    std::vector<Entry> entries_;
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
    static juce::Colour textDim()      { return juce::Colour(0xffaeb9c8); }
    static juce::Colour textFaint()    { return juce::Colour(0xff8b98ab); }
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

// ===========================================================================
// Shared route-table caps. GUI-editor cap only — the core is unbounded.
// File-scope so RouteTable (below) can use them before ProxyEditor exists.
// ===========================================================================
constexpr int kMaxRoutes = 64;
constexpr int kDefaultRoutes = 8;

// ===========================================================================
// Shared layout constants — the editor, its content component and the route
// table all derive their geometry from these (single source of truth).
// #8: the content is sized from contentH() and the editor window is capped
// at winCapH with a JUCE Viewport scrolling the overflow instead of the
// whole window growing off-screen.
// ===========================================================================
struct HeddleLayout {
    static constexpr int W = 860;
    static constexpr int headerH = 108;  // fixed top band in the editor (title bar + mode/listen row)
    // Content coordinates (the scrollable area starts below the fixed band):
    static constexpr int t0 = 16;        // TARGETS header top
    static constexpr int tRowH = 30;
    static int r0(int tRows) { return t0 + 36 + tRows * tRowH + 36 + 10; }
    static constexpr int rHeaderH = 40;  // route-table header zone (hdr + cols)
    static constexpr int rRowH = 36;
    static int tableH(int rRows) { return rHeaderH + rRows * rRowH + 6; }
    static int addRouteY(int tRows, int rRows) { return r0(tRows) + tableH(rRows); }
    static int d0(int tRows, int rRows) { return addRouteY(tRows, rRows) + 36 + 12; }
    static int contentH(int tRows, int rRows) { return d0(tRows, rRows) + 140 + 16; }
    static constexpr int winCapH = 760;  // window cap; the Viewport scrolls beyond
};

// ===========================================================================
// HeddleContent — the scrollable content (child of the editor's Viewport).
// Owns the background and the section dividers, which must travel with the
// content when it scrolls (#8). The title bar / mode / listen row live in the
// editor's FIXED header above the viewport and never scroll.
// ===========================================================================
class HeddleContent : public juce::Component {
public:
    HeddleContent() { setName("hcontent"); }
    void setCounts(int tRows, int rRows) {
        tRows_ = tRows;
        rRows_ = rRows;
        setSize(HeddleLayout::W, HeddleLayout::contentH(tRows, rRows));
    }
    void paint(juce::Graphics& g) override {
        g.setColour(HeddlePalette::base());
        g.fillRect(0, 0, getWidth(), getHeight());
        // Section dividers (positions from the same HeddleLayout the layout
        // code uses, so they can never drift out of sync).
        const int r0 = HeddleLayout::r0(tRows_);
        const int d0 = HeddleLayout::d0(tRows_, rRows_);
        g.setColour(HeddlePalette::divider());
        g.fillRect(16, HeddleLayout::t0 - 10, getWidth() - 32, 1);
        g.fillRect(16, r0 - 12, getWidth() - 32, 1);
        g.fillRect(16, d0 - 12, getWidth() - 32, 1);
        g.fillRect(668, d0 + 8, 1, 88);  // discover block | learn panel
    }

private:
    int tRows_ = 1;
    int rRows_ = kDefaultRoutes;
};

// ===========================================================================
// RouteTable — the routes table (header + rows) as ONE component that is
// ALSO the drag-and-drop target for the whole table.
//
// Drag-and-drop route editing (replaces double-click filling, #6): the
// DISCOVERED and RECEIVED ListBoxes are the drag sources (their models
// override ListBoxModel::getDragSourceDescription; JUCE's ListBox starts the
// drag on mouseDrag). The payload is a var (dragVarDiscovered /
// dragVarReceived). Dropping anywhere on a route row fills that row:
//   discovered line -> TGT (the line's target), SLOT, PARAM
//   received line   -> SURFACE ADDR, ARG (1 when the msg carries floats)
// JUCE's DragAndDropContainer::findTarget does getComponentAt() and walks UP
// the parent chain to the first interested DragAndDropTarget, so the row
// editors (TextEditor/ComboBox children of this component) never block the
// drop — the hit lands on an editor, walks up to this table.
//
// While a drag is over the table the hovered row is banded in the accent
// colour and the hovered column is boxed — the user sees exactly which cell
// will be set. The global LOCK switch (the editor's) rejects drops and
// disables every input at once.
// ===========================================================================
class RouteTable : public juce::Component,
                   public juce::DragAndDropTarget {
public:
    using DropCb = std::function<void(int /*row*/, const juce::var& /*payload*/)>;

    RouteTable() { setName("rt"); }

    void setDropCallback(DropCb cb) { cb_ = std::move(cb); }
    void setLocked(bool l) {
        locked_ = l;
        if (l) {
            hoverRow_ = -1;
            hoverCol_ = -1;
            repaint();
        }
    }
    // The editor owns the row controls and adds them to this table; it tells
    // us about them here so layout() can position them (table-local coords).
    void attachRow(int i, juce::TextEditor* src, juce::ComboBox* tgt,
                   juce::TextEditor* slot, juce::TextEditor* param,
                   juce::TextEditor* arg, juce::TextEditor* min,
                   juce::TextEditor* max, juce::ComboBox* curve,
                   juce::TextEditor* k, juce::Button* clr) {
        src_[i] = src; tgt_[i] = tgt; slot_[i] = slot; param_[i] = param;
        arg_[i] = arg; min_[i] = min; max_[i] = max; curve_[i] = curve;
        kEd_[i] = k; clr_[i] = clr;
    }
    void attachHeaderLabels(juce::Label* hdr, juce::Label* cSrc, juce::Label* cTgt,
                            juce::Label* cSlot, juce::Label* cParam, juce::Label* cArg,
                            juce::Label* cMin, juce::Label* cMax, juce::Label* cCurve,
                            juce::Label* cK) {
        hdr_ = hdr; cSrc_ = cSrc; cTgt_ = cTgt; cSlot_ = cSlot; cParam_ = cParam;
        cArg_ = cArg; cMin_ = cMin; cMax_ = cMax; cCurve_ = cCurve; cK_ = cK;
        layout();
    }
    void setActiveRows(int n) {
        activeRows_ = n;
        layout();
    }
    // Table-local column geometry — the single source for both layout() and
    // the drop-hover highlight. 10 columns: 9 data + the "x" remove button.
    // Total span 12..798 (content is 860 wide).
    struct Col { int x, w; };
    static const Col& col(int idx) {
        static const Col cols[10] = {
            {12, 148},    // SURFACE ADDR
            {166, 52},    // TGT
            {224, 112},   // SLOT
            {342, 138},   // PARAM
            {486, 38},    // ARG
            {530, 34},    // MIN
            {570, 44},    // MAX
            {620, 78},    // CURVE
            {704, 36},    // K (scurve curvature)
            {746, 40},    // x
        };
        return cols[idx];
    }
    void layout() {
        if (hdr_ != nullptr) hdr_->setBounds(12, 3, 700, 16);
        if (cSrc_ != nullptr) {
            cSrc_->setBounds(col(0).x, 24, col(0).w, 14);
            cTgt_->setBounds(col(1).x, 24, col(1).w, 14);
            cSlot_->setBounds(col(2).x, 24, col(2).w, 14);
            cParam_->setBounds(col(3).x, 24, col(3).w, 14);
            cArg_->setBounds(col(4).x, 24, col(4).w, 14);
            cMin_->setBounds(col(5).x, 24, col(5).w, 14);
            if (cMax_ == cMin_)
                // One combined "MIN  MAX" header label: span both columns.
                cMin_->setBounds(col(5).x, 24, col(6).x + col(6).w - col(5).x, 14);
            else
                cMax_->setBounds(col(6).x, 24, col(6).w, 14);
            cCurve_->setBounds(col(7).x, 24, col(7).w, 14);
            cK_->setBounds(col(8).x, 24, col(8).w, 14);
        }
        for (int i = 0; i < activeRows_; ++i) {
            const int y = HeddleLayout::rHeaderH + i * HeddleLayout::rRowH;
            src_[i]->setBounds(col(0).x, y, col(0).w, 24);
            tgt_[i]->setBounds(col(1).x, y, col(1).w, 26);
            slot_[i]->setBounds(col(2).x, y, col(2).w, 24);
            param_[i]->setBounds(col(3).x, y, col(3).w, 24);
            arg_[i]->setBounds(col(4).x, y, col(4).w - 2, 24);
            min_[i]->setBounds(col(5).x, y, col(5).w - 2, 24);
            max_[i]->setBounds(col(6).x, y, col(6).w - 2, 24);
            curve_[i]->setBounds(col(7).x, y, col(7).w, 26);
            kEd_[i]->setBounds(col(8).x, y, col(8).w - 2, 24);
            clr_[i]->setBounds(col(9).x, y + 2, col(9).w, 26);
        }
    }

    // ---- DragAndDropTarget ------------------------------------------------
    bool isInterestedInDragSource(const juce::DragAndDropTarget::SourceDetails& s)
        override {
        return !locked_ && isDragDescription(s.description);
    }
    void itemDragEnter(const juce::DragAndDropTarget::SourceDetails& s) override {
        updateHover(s.localPosition);
        repaint();
    }
    void itemDragMove(const juce::DragAndDropTarget::SourceDetails& s) override {
        updateHover(s.localPosition);
        repaint();
    }
    void itemDragExit(const juce::DragAndDropTarget::SourceDetails&) override {
        hoverRow_ = -1;
        hoverCol_ = -1;
        repaint();
    }
    void itemDropped(const juce::DragAndDropTarget::SourceDetails& s) override {
        const int row = rowFromY(s.localPosition.y);
        hoverRow_ = -1;
        hoverCol_ = -1;
        repaint();
        if (row < 0 || row >= activeRows_ || locked_) return;
        if (cb_) cb_(row, s.description);
    }

    void paint(juce::Graphics& g) override {
        // Subtle panel background behind the rows + row dividers.
        g.setColour(juce::Colour(0xff1d232c));
        g.fillRect(getLocalBounds());
        g.setColour(HeddlePalette::divider());
        for (int i = 0; i <= activeRows_; ++i) {
            const int y = HeddleLayout::rHeaderH + i * HeddleLayout::rRowH;
            if (y > getHeight()) break;
            g.fillRect(2, y - 1, getWidth() - 4, 1);
        }
        if (hoverRow_ < 0 || hoverRow_ >= activeRows_) return;
        const int y = HeddleLayout::rHeaderH + hoverRow_ * HeddleLayout::rRowH;
        const juce::Colour accent = HeddlePalette::accent();
        g.setColour(accent.withAlpha(0.14f));
        g.fillRect(0, y, getWidth(), HeddleLayout::rRowH);
        if (hoverCol_ >= 0) {
            const Col& c = col(hoverCol_);
            g.setColour(accent.withAlpha(0.30f));
            g.fillRect(c.x - 2, y + 1, c.w + 4, HeddleLayout::rRowH - 2);
            g.setColour(accent);
            g.drawRect((float) (c.x - 2), (float) (y + 1), (float) (c.w + 4),
                       (float) (HeddleLayout::rRowH - 2), 1.5f);
        }
    }

private:
    static int rowFromY(int y) {
        if (y < HeddleLayout::rHeaderH) return -1;
        return (y - HeddleLayout::rHeaderH) / HeddleLayout::rRowH;
    }
    void updateHover(const juce::Point<int>& p) {
        hoverRow_ = rowFromY(p.y);
        if (hoverRow_ < 0 || hoverRow_ >= activeRows_) {
            hoverRow_ = -1;
            hoverCol_ = -1;
            return;
        }
        hoverCol_ = -1;
        for (int i = 0; i < 10; ++i) {
            const Col& c = col(i);
            if (p.x >= c.x - 2 && p.x <= c.x + c.w + 2) {
                hoverCol_ = i;
                break;
            }
        }
    }

    DropCb cb_;
    int activeRows_ = 0;
    bool locked_ = false;
    int hoverRow_ = -1;
    int hoverCol_ = -1;

    juce::Label* hdr_ = nullptr;
    juce::Label* cSrc_ = nullptr;
    juce::Label* cTgt_ = nullptr;
    juce::Label* cSlot_ = nullptr;
    juce::Label* cParam_ = nullptr;
    juce::Label* cArg_ = nullptr;
    juce::Label* cMin_ = nullptr;
    juce::Label* cMax_ = nullptr;
    juce::Label* cCurve_ = nullptr;
    juce::Label* cK_ = nullptr;
    juce::TextEditor* src_[kMaxRoutes] = {};
    juce::ComboBox* tgt_[kMaxRoutes] = {};
    juce::TextEditor* slot_[kMaxRoutes] = {};
    juce::TextEditor* param_[kMaxRoutes] = {};
    juce::TextEditor* arg_[kMaxRoutes] = {};
    juce::TextEditor* min_[kMaxRoutes] = {};
    juce::TextEditor* max_[kMaxRoutes] = {};
    juce::ComboBox* curve_[kMaxRoutes] = {};
    juce::TextEditor* kEd_[kMaxRoutes] = {};
    juce::Button* clr_[kMaxRoutes] = {};
};

// ===========================================================================
// HeddleViewport — not needed: JUCE 8.0.4's stock Viewport registers a
// ComponentListener on the viewed component (setViewedComponent ->
// addComponentListener; componentMovedOrResized -> updateVisibleArea), so it
// already re-flows its scrollbars when the CONTENT resizes. A plain
// juce::Viewport is used for #8.
// ===========================================================================

// ===========================================================================
// #9 dirty tracking: this JUCE build has NO TextEditorListener and its
// TextEditor::textChanged() is not virtual, so event hooks are out. Instead
// the 4 Hz tick snapshots every field (fieldSnapshot()) and only runs
// readInputs() when the snapshot actually changed — "not if nothing changes".
// Programmatic field writes (drops, row add/remove) also change the
// snapshot, so they sync on the next tick automatically.
// ===========================================================================

class ProxyEditor : public juce::AudioProcessorEditor,
                    public juce::Timer,
                    public juce::DragAndDropContainer {
public:
    explicit ProxyEditor(ProxyProcessor& owner)
        : juce::AudioProcessorEditor(owner), owner_(owner) {
        // The tables are dynamic: routes start with the stored routes (min 8
        // so the default layout is familiar), targets with the stored targets
        // (min 1); ADD ROUTE / ADD TARGET grow them from there.
        numRows_ = (int) juce::jlimit((int) kDefaultRoutes, (int) kMaxRoutes,
                                      (int) std::max<size_t>(kDefaultRoutes,
                                                              owner_.cfg_.routes.size()));
        numTargets_ = std::max(1, (int) owner_.cfg_.targets.size());
        // The window is capped (HeddleLayout::winCapH); when the content
        // outgrows it the Viewport scrolls the overflow instead of the whole
        // window growing off-screen (#8).
        setSize(HeddleLayout::W, HeddleLayout::winCapH);
        toBack();

        // Scrollable content: everything below the fixed header band scrolls
        // with it (background + section dividers live on HeddleContent). The
        // window is capped; the viewport scrolls the overflow (#8).
        content_ = new HeddleContent();
        content_->setCounts(numTargets_, numRows_);
        viewport_ = new juce::Viewport("vp");
        viewport_->getVerticalScrollBar().setColour(juce::ScrollBar::thumbColourId,
                                                    HeddlePalette::panelFocus());
        viewport_->getVerticalScrollBar().setColour(juce::ScrollBar::trackColourId,
                                                    HeddlePalette::baseTop());
        viewport_->setViewedComponent(content_, false);
        addAndMakeVisible(viewport_);

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

        // Listen HOST: which interfaces the listen socket binds. Default
        // 0.0.0.0 (all) — required for a LAN surface (TouchOSC on a phone);
        // 127.0.0.1 for loopback-only setups.
        hostLbl_ = new juce::Label("lhst", "LISTEN HOST");
        styleFaintLabel(hostLbl_);
        addAndMakeVisible(hostLbl_);
        hostEd_ = new juce::TextEditor("lhp");
        hostEd_->setText(owner_.cfg_.listenHost.empty() ? juce::String("0.0.0.0")
                                                        : jstr(owner_.cfg_.listenHost));
        hostEd_->setJustification(juce::Justification::centredLeft);
        styleInput(hostEd_);
        addAndMakeVisible(hostEd_);

        // Global lock switch (prevents accidental edits, #6): ON disables
        // every input and rejects drag drops; the user asked for it together
        // with the drag-and-drop route filling.
        lockBtn_ = new HeddleButton("lock", "LOCK OFF", HeddleButton::Kind::Neutral);
        lockBtn_->onClick = [this] {
            locked_ = !locked_;
            lockBtn_->setButtonText(locked_ ? "LOCK ON" : "LOCK OFF");
            applyLock();
        };
        addAndMakeVisible(lockBtn_);

        buildTargets();
        buildRoutes();
        buildDiscover();
        buildLearn();

        // Poll the live counters + status; the 4 Hz tick also keeps cfg_
        // in sync with the typed fields so the DAW's state save
        // (getStateInformation) always captures the latest content (#9).
        // The ENGINE is only restarted on APPLY (commitConfig), so this is
        // cheap and never disturbs a running engine.
        startTimerHz(4);
        applyLock();
        refreshStatus();
        resized();  // layout now that all children exist (first setSize skipped it)
    }

    // #6 global lock: ON makes every field read-only and rejects drag drops,
    // so a running rig can't be re-pointed by an accidental keystroke or
    // drag. (This JUCE build has no setEditable; setReadOnly is the API.)
    void applyLock() {
        const bool on = locked_;
        if (listenEd_ != nullptr) listenEd_->setReadOnly(on);
        if (hostEd_ != nullptr) hostEd_->setReadOnly(on);
        for (size_t i = 0; i < targetIp_.size(); ++i) {
            if (targetIp_[i] != nullptr) targetIp_[i]->setReadOnly(on);
            if (targetPort_[i] != nullptr) targetPort_[i]->setReadOnly(on);
            if (targetEn_[i] != nullptr) targetEn_[i]->setEnabled(!on);
        }
        for (int i = 0; i < kMaxRoutes; ++i) {
            routeSrc_[i]->setReadOnly(on);
            routeSlot_[i]->setReadOnly(on);
            routeParam_[i]->setReadOnly(on);
            routeArg_[i]->setReadOnly(on);
            routeMin_[i]->setReadOnly(on);
            routeMax_[i]->setReadOnly(on);
            routeK_[i]->setReadOnly(on);
            routeTgt_[i]->setEnabled(!on);
            routeCurve_[i]->setEnabled(!on);
        }
        if (routeTable_ != nullptr) routeTable_->setLocked(on);
        if (addTargetBtn_ != nullptr) addTargetBtn_->setEnabled(!on);
        if (addRouteBtn_ != nullptr) addRouteBtn_->setEnabled(!on);
    }

    ~ProxyEditor() override {
        // #9: final sync of the on-screen fields into cfg_. The DAW may have
        // already taken its state snapshot, and the 4 Hz timer keeps cfg_
        // current regardless — this is belt-and-braces for a save taken
        // between the last tick and the window close.
        readInputs();
    }

    void timerCallback() override {
        // Keep target dropdowns inside the live row count (a route row's TGT
        // or the DISCOVER combo may predate a removed target row).
        const int tN = std::max(1, (int) targetEn_.size());
        for (int i = 0; i < kMaxRoutes; ++i)
            if (routeTgt_[i]->getSelectedId() > tN)
                routeTgt_[i]->setSelectedId(tN, juce::dontSendNotification);
        if (discTgt_->getSelectedId() > tN)
            discTgt_->setSelectedId(tN, juce::dontSendNotification);
        // #9: keep cfg_ in sync with the fields the user is looking at, so
        // ANY DAW state save (getStateInformation) captures the current
        // route content and count — but ONLY when a field actually changed
        // ("not if nothing changes"): snapshot every field and compare.
        const std::string snap = fieldSnapshot();
        if (snap != lastSnap_) {
            lastSnap_ = std::move(snap);
            readInputs();
        }
        refreshStatus();
        refreshLearn();
    }

    void paint(juce::Graphics& g) override {
        // The editor only paints its FIXED header band (0..headerH). Everything
        // below scrolls inside the Viewport and is drawn by HeddleContent (its
        // own background + section dividers travel with the scroll, #8).
        const int h = HeddleLayout::headerH;
        g.setColour(HeddlePalette::base());
        g.fillRect(0, 0, getWidth(), h);
        // Subtle top gradient over the header band (1px interpolated lines —
        // this JUCE build's Graphics has no brush/gradient fill API).
        const juce::Colour top = HeddlePalette::baseTop();
        const juce::Colour bot = HeddlePalette::base();
        for (int y = 0; y < h; ++y) {
            const float t = (float)y / (float)h;
            const juce::uint8 r  = (juce::uint8)(top.getRed()   + t * (bot.getRed()   - top.getRed()));
            const juce::uint8 gr = (juce::uint8)(top.getGreen() + t * (bot.getGreen() - top.getGreen()));
            const juce::uint8 b  = (juce::uint8)(top.getBlue()  + t * (bot.getBlue()  - top.getBlue()));
            g.setColour(juce::Colour::fromRGB(r, gr, b));
            g.fillRect(0, y, getWidth(), 1);
        }
        // Title-bar accent underline + the divider between the fixed header
        // and the scrollable content.
        g.setColour(HeddlePalette::accent());
        g.fillRoundedRectangle(16, 44, 44, 4, 2.0f);
        g.setColour(HeddlePalette::divider());
        g.fillRect(0, h - 1, getWidth(), 1);
    }

    void resized() override {
        // The editor is a FIXED window (capped at winCapH). Only the header
        // band (title/status/mode/listen/lock) is laid out here; the rest of
        // the controls are children of HeddleContent, which lives inside the
        // Viewport below the band and scrolls when the content outgrows the
        // window (#8).
        if (title_ == nullptr || targetsHdr_ == nullptr ||
            routesHdr_ == nullptr || discLbl_ == nullptr)
            return;

        const int hh = HeddleLayout::headerH;
        // ---- Fixed header band (0 .. headerH) ------------------------------
        title_->setBounds(74, 12, 380, 24);
        status_->setBounds(getWidth() - 332, 14, 316, 18);
        mode_->setBounds(16, 62, 170, 30);
        lockBtn_->setBounds(196, 62, 104, 30);
        apply_->setBounds(getWidth() - 106, 62, 90, 30);
        listenLbl_->setBounds(320, 68, 96, 20);
        listenEd_->setBounds(420, 66, 74, 24);
        hostLbl_->setBounds(512, 68, 92, 20);
        hostEd_->setBounds(612, 66, 96, 24);

        // ---- Scrollable content (viewport below the band) ------------------
        // content_ owns the targets/routes/discover/learn controls; position
        // the viewport to fill the area under the fixed band. The content is
        // already sized by setCounts() (contentH), so if it is taller than
        // the viewport JUCE shows the scrollbars (HeddleViewport re-flows it
        // on every content resize, not just its own).
        viewport_->setBounds(0, hh, getWidth(), getHeight() - hh);
        content_->setSize(HeddleLayout::W, HeddleLayout::contentH(numTargets_, numRows_));

        // Targets block (content-local coordinates; t0 sits just under the band)
        const int t0 = HeddleLayout::t0;
        const int tRowH = HeddleLayout::tRowH;
        targetsHdr_->setBounds(16, t0, 360, 16);
        tgtColEn_->setBounds(16, t0 + 18, 60, 14);
        tgtColIp_->setBounds(86, t0 + 18, 150, 14);
        tgtColPt_->setBounds(242, t0 + 18, 64, 14);
        for (size_t i = 0; i < targetEn_.size(); ++i) {
            const int y = t0 + 36 + (int) i * tRowH;
            targetEn_[i]->setBounds(28, y + 4, 40, 20);
            targetIp_[i]->setBounds(86, y, 150, 24);
            targetPort_[i]->setBounds(242, y, 64, 24);
            targetClr_[i]->setBounds(316, y + 2, 48, 26);
        }
        addTargetBtn_->setBounds(16, t0 + 36 + numTargets_ * tRowH, 110, 28);

        // Routes table: one component at r0, height = header + rows (+ pad).
        // The table lays out its own columns/rows (layout() in RouteTable).
        const int r0 = HeddleLayout::r0(numTargets_);
        routesHdr_->setBounds(12, r0 - 30, 700, 16);
        routeTable_->setBounds(0, r0, HeddleLayout::W,
                               HeddleLayout::tableH(numRows_));
        routeTable_->setActiveRows(numRows_);
        addRouteBtn_->setBounds(16, HeddleLayout::addRouteY(numTargets_, numRows_),
                                110, 28);

        // Discover block + learn panel (both under the routes table).
        const int d0 = HeddleLayout::d0(numTargets_, numRows_);
        learnHdr_->setBounds(688, d0 + 8, 160, 16);
        learnList_->setBounds(688, d0 + 28, 156, 70);
        discHdr_->setBounds(16, d0 + 8, 400, 16);
        discBtn_->setBounds(16, d0 + 32, 120, 30);
        discTgt_->setBounds(152, d0 + 32, 104, 28);
        discList_->setBounds(16, d0 + 70, 300, 70);
        discLbl_->setBounds(332, d0 + 70, 320, 70);
    }

private:
    // Hard cap on route rows. The table was a fixed 8; it is now dynamic —
    // ADD ROUTE appends, each row's "x" removes it, and the editor resizes.
    static constexpr int kMaxRoutes = 64;
    static constexpr int kDefaultRoutes = 8;

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
    // Fonts are deliberately bigger and higher-contrast than 0.1.8: 9pt faint
    // labels on #171b21 were unreadable, and the list text was the same.
    static void styleFaintLabel(juce::Label* l) {
        l->setFont(hf(10.5f, juce::Font::bold));
        l->setColour(juce::Label::textColourId, HeddlePalette::textDim());
    }
    static void styleInput(juce::TextEditor* e) {
        // #4 fix: setFont() alone does NOT re-style text that was already
        // inserted — the section keeps the font it was added with, so a field
        // filled before its font was applied (or resized by a re-layout) shows
        // pre-existing text at the old size while newly typed characters use
        // the new one (the "little 0" bug). applyFontToAllText() re-styles
        // every section with the current font.
        e->setFont(hf(13.5f));
        e->applyFontToAllText(hf(13.5f));
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

    // ---- Dynamic target + route tables (add / remove / sizing) -----------
    // Keep every target dropdown (route TGT combos + the DISCOVER combo) in
    // sync with the live target rows. Row i always offers t0..t(i-1), so the
    // dropdowns only ever point at rows that exist; the 4 Hz timer applies the
    // same rule, so a stale selection is self-correcting.
    void rebuildTargetItems() {
        for (int i = 0; i < kMaxRoutes; ++i) {
            const int sel = routeTgt_[i]->getSelectedId();
            routeTgt_[i]->clear(juce::dontSendNotification);
            for (int k = 0; k < (int) targetEn_.size(); ++k)
                routeTgt_[i]->addItem("t" + juce::String(k), k + 1);
            routeTgt_[i]->setSelectedId(juce::jlimit(1, (int) targetEn_.size(), sel),
                                        juce::dontSendNotification);
        }
        const int dSel = discTgt_->getSelectedId();
        discTgt_->clear(juce::dontSendNotification);
        for (int k = 0; k < (int) targetEn_.size(); ++k)
            discTgt_->addItem("t" + juce::String(k), k + 1);
        discTgt_->setSelectedId(juce::jlimit(1, (int) targetEn_.size(), dSel),
                                juce::dontSendNotification);
    }
    // Append an empty target row.
    void addTargetRow() {
        targetEn_.push_back(new juce::ToggleButton("te" + juce::String(targetEn_.size())));
        auto* en = targetEn_.back();
        en->setButtonText("ON");
        en->setToggleState(true, juce::dontSendNotification);
        en->setClickingTogglesState(true);
        en->setColour(juce::ToggleButton::textColourId, HeddlePalette::text());
        en->setColour(juce::ToggleButton::tickColourId, HeddlePalette::accent());
        content_->addAndMakeVisible(en);
        targetIp_.push_back(new juce::TextEditor("ti" + juce::String(targetIp_.size())));
        targetIp_.back()->setJustification(juce::Justification::centredLeft);
        styleInput(targetIp_.back());
        content_->addAndMakeVisible(targetIp_.back());
        targetPort_.push_back(new juce::TextEditor("tp" + juce::String(targetPort_.size())));
        targetPort_.back()->setJustification(juce::Justification::centredLeft);
        styleInput(targetPort_.back());
        content_->addAndMakeVisible(targetPort_.back());
        const int row = (int) targetEn_.size() - 1;
        targetClr_.push_back(new HeddleButton("tx" + juce::String(row), "x",
                                              HeddleButton::Kind::Neutral));
        targetClr_.back()->onClick = [this, row] {
            if (locked_) return;
            removeTargetRow(row);
        };
        content_->addAndMakeVisible(targetClr_.back());
        ++numTargets_;
        refreshLayout();
        status_->setText("target row added - press APPLY", juce::dontSendNotification);
    }
    // Remove a target row: destroy its controls and shift the rows below up
    // (each keeps its contents). Routes pointing at removed/higher indexes
    // are clamped on APPLY.
    void removeTargetRow(int row) {
        if (row < 0 || row >= (int) targetEn_.size()) return;
        for (int i = row; i + 1 < (int) targetEn_.size(); ++i) {
            const bool en = targetEn_[i + 1]->getToggleState();
            const juce::String ip = targetIp_[i + 1]->getText();
            const juce::String pt = targetPort_[i + 1]->getText();
            targetEn_[i]->setToggleState(en, juce::dontSendNotification);
            targetIp_[i]->setText(ip);
            targetPort_[i]->setText(pt);
        }
        delete targetEn_.back();
        delete targetIp_.back();
        delete targetPort_.back();
        delete targetClr_.back();
        targetEn_.pop_back();
        targetIp_.pop_back();
        targetPort_.pop_back();
        targetClr_.pop_back();
        if (numTargets_ > 0) --numTargets_;
        refreshLayout();
        status_->setText("target row removed - press APPLY", juce::dontSendNotification);
    }
    // Re-layout everything after a row was added/removed. The window stays
    // capped; growing the content beyond it just shows the viewport's
    // scrollbars (HeddleViewport re-flows on the content resize).
    void refreshLayout() {
        rebuildTargetItems();
        content_->setCounts(numTargets_, numRows_);
        resized();
    }
    // Show / hide one whole route row's 9 controls. All kMaxRoutes rows are
    // built up front (so ADD ROUTE never dereferences a null control), but
    // only the active `numRows_` are visible; the rest are added to the
    // hierarchy but hidden. Re-shown on ADD, hidden on remove (the vacated
    // row's controls otherwise linger on screen — the "ghost row" bug).
    void setRouteRowVisible(int row, bool vis) {
        if (row < 0 || row >= kMaxRoutes) return;
        routeSrc_[row]->setVisible(vis);
        routeTgt_[row]->setVisible(vis);
        routeSlot_[row]->setVisible(vis);
        routeParam_[row]->setVisible(vis);
        routeArg_[row]->setVisible(vis);
        routeMin_[row]->setVisible(vis);
        routeMax_[row]->setVisible(vis);
        routeCurve_[row]->setVisible(vis);
        routeK_[row]->setVisible(vis);
        routeClr_[row]->setVisible(vis);
    }
    // Set every control of route row `row` from a config route (defaults if
    // null) — shared by ctor fill, ADD ROUTE and remove-row shifting.
    void setRouteRow(int row, const ProxyRoute* r) {
        routeSrc_[row]->setText(r ? jstr(r->srcAddr) : juce::String());
        const int tN = std::max(1, (int) targetEn_.size());
        routeTgt_[row]->setSelectedId(r ? juce::jlimit(1, tN, r->targetIdx + 1) : 1,
                                      juce::dontSendNotification);
        routeSlot_[row]->setText(r ? jstr(r->slot) : juce::String());
        routeParam_[row]->setText(r ? jstr(r->param) : juce::String());
        routeArg_[row]->setText(r ? juce::String(r->arg) : juce::String("0"));
        routeMin_[row]->setText(r ? fmtDbl(r->inMin) : juce::String("0"));
        routeMax_[row]->setText(r ? fmtDbl(r->inMax) : juce::String("1"));
        routeCurve_[row]->setSelectedId(r ? idFromCurve(r->curve) : 1,
                                        juce::dontSendNotification);
        routeK_[row]->setText(r ? fmtDbl(r->curvature) : juce::String("1"));
    }
    void addRouteRow() {
        if (numRows_ >= kMaxRoutes) {
            status_->setText("route table is full (" + juce::String(kMaxRoutes)
                                 + " max) — remove a row first",
                             juce::dontSendNotification);
            return;
        }
        setRouteRow(numRows_, nullptr);
        setRouteRowVisible(numRows_, true);   // reveal the pre-built spare row
        ++numRows_;
        refreshLayout();
        status_->setText("route row added - press APPLY", juce::dontSendNotification);
    }
    // Remove a row: shift the rows below it up (each keeps its contents),
    // shrink the table by one. The row's controls are reused, not deleted.
    void removeRouteRow(int row) {
        if (row < 0 || row >= numRows_) return;
        for (int i = row; i + 1 < numRows_; ++i) {
            const ProxyRoute tmp = routeRowAt(i + 1);
            setRouteRow(i, &tmp);
        }
        setRouteRow(numRows_ - 1, nullptr);
        setRouteRowVisible(numRows_ - 1, false);  // hide the vacated row
        --numRows_;
        if (numRows_ < 1) numRows_ = 1;
        refreshLayout();
        status_->setText("route row removed - press APPLY", juce::dontSendNotification);
    }
    // Read a row's controls back into a ProxyRoute (used when shifting).
    ProxyRoute routeRowAt(int i) {
        ProxyRoute r;
        const juce::String src = routeSrc_[i]->getText().trim();
        const juce::String slot = routeSlot_[i]->getText().trim();
        const juce::String param = routeParam_[i]->getText().trim();
        r.srcAddr = src.isEmpty() ? std::string()
                                  : (src.startsWith("/") ? src : ("/" + src)).toStdString();
        int ti = routeTgt_[i]->getSelectedId() - 1;
        if (ti < 0) ti = 0;
        r.targetIdx = ti;
        r.slot = slot.toStdString();
        r.param = param.toStdString();
        r.arg = parseArg(routeArg_[i]->getText(), 0);
        r.inMin = parseDbl(routeMin_[i]->getText(), 0.0);
        r.inMax = parseDbl(routeMax_[i]->getText(), 1.0);
        r.curve = curveFromId(routeCurve_[i]->getSelectedId());
        r.curvature = parseDbl(routeK_[i]->getText(), 1.0);
        return r;
    }

    // Copy editor field values into owner_.cfg_ (targets + routes + listen).
    void readInputs() {
        std::string err;
        int port = listenEd_->getText().getIntValue();
        if (port > 0 && port <= 65535) owner_.cfg_.listenPort = port;
        // Listen host: empty field = all interfaces (0.0.0.0).
        const juce::String h = hostEd_->getText().trim();
        owner_.cfg_.listenHost = h.isEmpty() ? "0.0.0.0" : h.toStdString();

        // Targets: rebuild from the live rows (a row is present if it has an
        // ip or port value).
        std::vector<ProxyTarget> ts;
        for (size_t i = 0; i < targetEn_.size(); ++i) {
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
        for (int i = 0; i < numRows_; ++i) {
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
            r.curvature = parseDbl(routeK_[i]->getText(), 1.0);
            if (!r.slot.empty() && !r.param.empty())
                rs.push_back(std::move(r));
        }
        owner_.cfg_.routes = std::move(rs);
        (void) err;
    }

    void buildTargets() {
        targetsHdr_ = new juce::Label("th", "TARGETS");
        styleFaintLabel(targetsHdr_);
        content_->addAndMakeVisible(targetsHdr_);
        tgtColEn_ = new juce::Label("tce", "ON");
        styleFaintLabel(tgtColEn_);
        content_->addAndMakeVisible(tgtColEn_);
        tgtColIp_ = new juce::Label("tci", "HOST / IP");
        styleFaintLabel(tgtColIp_);
        content_->addAndMakeVisible(tgtColIp_);
        tgtColPt_ = new juce::Label("tcp", "PORT");
        styleFaintLabel(tgtColPt_);
        content_->addAndMakeVisible(tgtColPt_);
        // One live row per stored target (min 1); ADD TARGET appends more.
        for (int i = 0; i < numTargets_; ++i) {
            targetEn_.push_back(new juce::ToggleButton("te" + juce::String(i)));
            auto* en = targetEn_.back();
            en->setButtonText("ON");
            en->setToggleState(owner_.cfg_.targets.size() > i ? owner_.cfg_.targets[i].enabled : true,
                               juce::dontSendNotification);
            en->setClickingTogglesState(true);
            en->setColour(juce::ToggleButton::textColourId, HeddlePalette::text());
            en->setColour(juce::ToggleButton::tickColourId, HeddlePalette::accent());
            en->onClick = [this, i] { (void) i; };
            content_->addAndMakeVisible(en);
            targetIp_.push_back(new juce::TextEditor("ti" + juce::String(i)));
            targetIp_.back()->setJustification(juce::Justification::centredLeft);
            styleInput(targetIp_.back());
            content_->addAndMakeVisible(targetIp_.back());
            targetPort_.push_back(new juce::TextEditor("tp" + juce::String(i)));
            targetPort_.back()->setJustification(juce::Justification::centredLeft);
            styleInput(targetPort_.back());
            content_->addAndMakeVisible(targetPort_.back());
            if (owner_.cfg_.targets.size() > i) {
                targetIp_.back()->setText(jstr(owner_.cfg_.targets[i].ip));
                targetPort_.back()->setText(juce::String(owner_.cfg_.targets[i].port));
            }
            const int tRow = i;
            targetClr_.push_back(new HeddleButton("tx" + juce::String(tRow), "x",
                                                  HeddleButton::Kind::Neutral));
            targetClr_.back()->onClick = [this, tRow] {
                if (locked_) return;
                removeTargetRow(tRow);
            };
            content_->addAndMakeVisible(targetClr_.back());
        }
        addTargetBtn_ = new HeddleButton("atd", "+ ADD TARGET", HeddleButton::Kind::Neutral);
        addTargetBtn_->onClick = [this] {
            if (locked_) return;
            addTargetRow();
        };
        content_->addAndMakeVisible(addTargetBtn_);
    }

    void buildRoutes() {
        routesHdr_ = new juce::Label("rh", "ROUTES   (drag a line from DISCOVERED/RECEIVED onto a row, or type; LOCK stops edits)");
        styleFaintLabel(routesHdr_);
        content_->addAndMakeVisible(routesHdr_);
        // The table is one component = one drop target and the PARENT of all
        // row controls (see the RouteTable class note). Its column header
        // labels are children of the table so they scroll with it.
        routeTable_ = new RouteTable();
        content_->addAndMakeVisible(routeTable_);
        rtColSrc_ = new juce::Label("rcs", "SURFACE ADDR");
        styleFaintLabel(rtColSrc_);
        routeTable_->addAndMakeVisible(rtColSrc_);
        rtColTgt_ = new juce::Label("rct", "TGT");
        styleFaintLabel(rtColTgt_);
        routeTable_->addAndMakeVisible(rtColTgt_);
        rtColSlot_ = new juce::Label("rcsl", "SLOT");
        styleFaintLabel(rtColSlot_);
        routeTable_->addAndMakeVisible(rtColSlot_);
        rtColPar_ = new juce::Label("rcp", "PARAM");
        styleFaintLabel(rtColPar_);
        routeTable_->addAndMakeVisible(rtColPar_);
        rtColArg_ = new juce::Label("rca", "ARG");
        styleFaintLabel(rtColArg_);
        routeTable_->addAndMakeVisible(rtColArg_);
        rtColMM_ = new juce::Label("rcmm", "MIN  MAX");
        styleFaintLabel(rtColMM_);
        routeTable_->addAndMakeVisible(rtColMM_);
        rtColCv_ = new juce::Label("rcc", "CURVE");
        styleFaintLabel(rtColCv_);
        routeTable_->addAndMakeVisible(rtColCv_);
        rtColK_ = new juce::Label("rck", "K");
        styleFaintLabel(rtColK_);
        routeTable_->addAndMakeVisible(rtColK_);
        routeTable_->attachHeaderLabels(nullptr, rtColSrc_, rtColTgt_, rtColSlot_,
                                        rtColPar_, rtColArg_, rtColMM_, rtColMM_,
                                        rtColCv_, rtColK_);
        // Target combo items: t0..t(n-1) for the live target rows. Rows are
        // built after buildTargets(), so targetEn_.size() is final here;
        // ADD/REMOVE TARGET keeps the combos in sync via rebuildTargetItems().
        // Build ALL kMaxRoutes rows up front. ADD ROUTE reveals a pre-built
        // spare (it never constructs one at runtime, which is what used to
        // dereference a null control and crash the host); only the first
        // numRows_ are shown, the rest are added-but-hidden.
        for (int i = 0; i < kMaxRoutes; ++i) {
            routeSrc_[i] = new juce::TextEditor("rs" + juce::String(i));
            if (owner_.cfg_.routes.size() > i)
                routeSrc_[i]->setText(jstr(owner_.cfg_.routes[i].srcAddr));
            routeSrc_[i]->setJustification(juce::Justification::centredLeft);
            styleInput(routeSrc_[i]);
            routeTable_->addAndMakeVisible(routeSrc_[i]);

            routeTgt_[i] = new juce::ComboBox("rt" + juce::String(i));
            const int tN = std::max(1, (int) targetEn_.size());
            for (int k = 0; k < tN; ++k) routeTgt_[i]->addItem("t" + juce::String(k), k + 1);
            const int sel = owner_.cfg_.routes.size() > i ? owner_.cfg_.routes[i].targetIdx + 1 : 1;
            routeTgt_[i]->setSelectedId(juce::jlimit(1, tN, sel), juce::dontSendNotification);
            styleCombo(routeTgt_[i]);
            routeTable_->addAndMakeVisible(routeTgt_[i]);

            routeSlot_[i] = new juce::TextEditor("ro" + juce::String(i));
            if (owner_.cfg_.routes.size() > i)
                routeSlot_[i]->setText(jstr(owner_.cfg_.routes[i].slot));
            routeSlot_[i]->setJustification(juce::Justification::centredLeft);
            styleInput(routeSlot_[i]);
            routeTable_->addAndMakeVisible(routeSlot_[i]);

            routeParam_[i] = new juce::TextEditor("rp" + juce::String(i));
            if (owner_.cfg_.routes.size() > i)
                routeParam_[i]->setText(jstr(owner_.cfg_.routes[i].param));
            routeParam_[i]->setJustification(juce::Justification::centredLeft);
            styleInput(routeParam_[i]);
            routeTable_->addAndMakeVisible(routeParam_[i]);

            // Value mapping for this route (ARG / MIN / MAX / CURVE / K).
            const ProxyRoute* rt = owner_.cfg_.routes.size() > i ? &owner_.cfg_.routes[i] : nullptr;
            routeArg_[i] = new juce::TextEditor("ra" + juce::String(i));
            routeArg_[i]->setText(rt ? juce::String(rt->arg) : juce::String("0"));
            routeArg_[i]->setJustification(juce::Justification::centred);
            styleInput(routeArg_[i]);
            routeTable_->addAndMakeVisible(routeArg_[i]);

            routeMin_[i] = new juce::TextEditor("rm" + juce::String(i));
            routeMin_[i]->setText(rt ? fmtDbl(rt->inMin) : juce::String("0"));
            routeMin_[i]->setJustification(juce::Justification::centred);
            styleInput(routeMin_[i]);
            routeTable_->addAndMakeVisible(routeMin_[i]);

            routeMax_[i] = new juce::TextEditor("rxm" + juce::String(i));
            routeMax_[i]->setText(rt ? fmtDbl(rt->inMax) : juce::String("1"));
            routeMax_[i]->setJustification(juce::Justification::centred);
            styleInput(routeMax_[i]);
            routeTable_->addAndMakeVisible(routeMax_[i]);

            routeCurve_[i] = new juce::ComboBox("rc" + juce::String(i));
            static const char* kCurveNames[] = {"identity", "exp", "invexp", "scurve"};
            for (int c = 0; c < 4; ++c) routeCurve_[i]->addItem(kCurveNames[c], c + 1);
            const int cSel = rt ? idFromCurve(rt->curve) : 1;
            routeCurve_[i]->setSelectedId(juce::jlimit(1, 4, cSel), juce::dontSendNotification);
            styleCombo(routeCurve_[i]);
            routeTable_->addAndMakeVisible(routeCurve_[i]);

            // K: scurve curvature (#7). 0 = straight S (smoothstep once),
            // 1 = default, >1 = doubled smoothstep (sharper knee).
            routeK_[i] = new juce::TextEditor("rk" + juce::String(i));
            routeK_[i]->setText(rt ? fmtDbl(rt->curvature) : juce::String("1"));
            routeK_[i]->setJustification(juce::Justification::centred);
            styleInput(routeK_[i]);
            routeTable_->addAndMakeVisible(routeK_[i]);

            const int row = i;
            routeClr_[i] = new HeddleButton("rx" + juce::String(i), "x",
                                            HeddleButton::Kind::Neutral);
            routeClr_[i]->onClick = [this, row] {
                if (locked_) return;
                removeRouteRow(row);
            };
            routeTable_->addAndMakeVisible(routeClr_[i]);

            routeTable_->attachRow(i, routeSrc_[i], routeTgt_[i], routeSlot_[i],
                                   routeParam_[i], routeArg_[i], routeMin_[i],
                                   routeMax_[i], routeCurve_[i], routeK_[i],
                                   routeClr_[i]);
            setRouteRowVisible(i, i < numRows_);  // hide the spares
        }
        routeTable_->setActiveRows(numRows_);
        routeTable_->setDropCallback(
            [this](int row, const juce::var& d) { dropOnRoute(row, d); });

        // ADD ROUTE — append a row below the table (the table was a fixed 8).
        addRouteBtn_ = new HeddleButton("addr", "+ ADD ROUTE", HeddleButton::Kind::Neutral);
        addRouteBtn_->onClick = [this] {
            if (locked_) return;
            addRouteRow();
        };
        content_->addAndMakeVisible(addRouteBtn_);
    }
    // Drop payload -> one route row. `discovered` fills TGT+SLOT+PARAM;
    // `received` fills SURFACE ADDR (+ARG when the msg carries several floats,
    // so a multi-value control routes on its second value — adjust if the
    // first float is the value you want).
    void dropOnRoute(int row, const juce::var& d) {
        const juce::var t = d.getProperty("type", juce::var());
        if (row < 0 || row >= numRows_) return;
        if (t == "discovered") {
            const int tIdx = (int) d.getProperty("targetIdx", 0);
            routeTgt_[row]->setSelectedId(juce::jlimit(1, std::max(1, (int) targetEn_.size()),
                                                      tIdx + 1),
                                          juce::dontSendNotification);
            routeSlot_[row]->setText(d.getProperty("slot", juce::String()));
            routeParam_[row]->setText(d.getProperty("param", juce::String()));
        } else if (t == "received") {
            routeSrc_[row]->setText(d.getProperty("addr", juce::String()));
            const int nf = (int) d.getProperty("numFloats", 0);
            routeArg_[row]->setText(juce::String(nf > 1 ? 1 : 0));
        } else {
            return;
        }
        status_->setText("route row " + juce::String(row) + " set",
                         juce::dontSendNotification);
    }

    void buildDiscover() {
        discHdr_ = new juce::Label("dh", "DISCOVERED PARAMS");
        styleFaintLabel(discHdr_);
        content_->addAndMakeVisible(discHdr_);

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
            status_->setText(juce::String(d.params.size()) + " params discovered (drag a line onto a route row)",
                             juce::dontSendNotification);
        };
        content_->addAndMakeVisible(discBtn_);

        discTgt_ = new juce::ComboBox("dt");
        for (int k = 0; k < (int) targetEn_.size(); ++k)
            discTgt_->addItem("t" + juce::String(k), k + 1);
        discTgt_->setSelectedId(1, juce::dontSendNotification);
        styleCombo(discTgt_);
        content_->addAndMakeVisible(discTgt_);

        discList_ = new juce::ListBox("dl", &discModel_);
        discList_->setRowHeight(16);
        styleList(discList_);
        content_->addAndMakeVisible(discList_);

        discLbl_ = new juce::Label("dlb",
                                   "DISCOVER queries a target's slots / params over the\n"
                                   "Heddle OSC protocol (/chain/query). Drag a line from\n"
                                   "the list onto any route row — TGT, SLOT and PARAM\n"
                                   "are filled from that one gesture.");
        discLbl_->setFont(hf(10.5f));
        discLbl_->setColour(juce::Label::textColourId, HeddlePalette::textFaint());
        content_->addAndMakeVisible(discLbl_);
    }

    // ---- Learn panel -------------------------------------------------------
    // RECEIVED lists the OSC addresses the proxy has actually seen from the
    // surface (newest first, deduped) — so a user never has to guess what
    // TouchOSC sends. Double-click a line to fill the next empty route row's
    // SURFACE ADDR and ARG (arg = 1 when the message carries several floats,
    // so a multi-value control is routed on its second value — adjust if the
    // first float is the value you want).
    void buildLearn() {
        learnHdr_ = new juce::Label("lh", "RECEIVED (drag onto a route row)");
        styleFaintLabel(learnHdr_);
        content_->addAndMakeVisible(learnHdr_);

        learnList_ = new juce::ListBox("rl", &learnModel_);
        learnList_->setRowHeight(16);
        styleList(learnList_);
        content_->addAndMakeVisible(learnList_);
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
    juce::Label* hostLbl_ = nullptr;
    juce::TextEditor* hostEd_ = nullptr;
    HeddleButton* lockBtn_ = nullptr;
    bool locked_ = false;
    HeddleContent* content_ = nullptr;
    juce::Viewport* viewport_ = nullptr;
    RouteTable* routeTable_ = nullptr;

    juce::Label* targetsHdr_ = nullptr;
    juce::Label* tgtColEn_ = nullptr;
    juce::Label* tgtColIp_ = nullptr;
    juce::Label* tgtColPt_ = nullptr;
    // Dynamic target rows (unbounded — a normal rig has hundreds). The row
    // controls are created/destroyed by ADD TARGET / row "x".
    std::vector<juce::ToggleButton*> targetEn_;
    std::vector<juce::TextEditor*> targetIp_;
    std::vector<juce::TextEditor*> targetPort_;
    std::vector<juce::Button*> targetClr_;
    juce::Button* addTargetBtn_ = nullptr;
    int numTargets_ = 0;  // ACTIVE target rows

    juce::Label* routesHdr_ = nullptr;
    juce::Label* rtColSrc_ = nullptr;
    juce::Label* rtColTgt_ = nullptr;
    juce::Label* rtColSlot_ = nullptr;
    juce::Label* rtColPar_ = nullptr;
    juce::Label* rtColArg_ = nullptr;
    juce::Label* rtColMM_ = nullptr;
    juce::Label* rtColCv_ = nullptr;
    juce::Label* rtColK_ = nullptr;
    juce::TextEditor* routeSrc_[kMaxRoutes] = {};
    juce::ComboBox* routeTgt_[kMaxRoutes] = {};
    juce::TextEditor* routeSlot_[kMaxRoutes] = {};
    juce::TextEditor* routeParam_[kMaxRoutes] = {};
    juce::TextEditor* routeArg_[kMaxRoutes] = {};
    juce::TextEditor* routeMin_[kMaxRoutes] = {};
    juce::TextEditor* routeMax_[kMaxRoutes] = {};
    juce::ComboBox* routeCurve_[kMaxRoutes] = {};
    juce::TextEditor* routeK_[kMaxRoutes] = {};
    juce::Button* routeClr_[kMaxRoutes] = {};
    juce::Button* addRouteBtn_ = nullptr;
    int numRows_ = 0;  // ACTIVE route rows (kDefaultRoutes..kMaxRoutes)

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

    // #9: last field snapshot; timerCallback re-reads only when it changes.
    std::string lastSnap_;
    // Every field value concatenated with 0x1F separators. Raw UTF-8 is
    // appended straight to a std::string (no juce::String round-trip — that
    // constructor treats bytes as UTF-16). ~KB at 64 routes, cheap at 4 Hz.
    std::string fieldSnapshot() {
        std::string s;
        s.reserve(4096);
        auto put = [&s](const juce::String& v) {
            s.append(v.toRawUTF8(), (size_t) v.getNumBytesAsUTF8());
            s.push_back('\x1f');
        };
        put(listenEd_->getText());
        put(hostEd_->getText());
        for (size_t i = 0; i < targetEn_.size(); ++i) {
            put(targetIp_[i]->getText());
            put(targetPort_[i]->getText());
            put(juce::String(targetEn_[i]->getToggleState() ? "1" : "0"));
        }
        put(juce::String(numRows_));
        for (int i = 0; i < numRows_; ++i) {
            put(routeSrc_[i]->getText());
            put(juce::String(routeTgt_[i]->getSelectedId()));
            put(routeSlot_[i]->getText());
            put(routeParam_[i]->getText());
            put(routeArg_[i]->getText());
            put(routeMin_[i]->getText());
            put(routeMax_[i]->getText());
            put(juce::String(routeCurve_[i]->getSelectedId()));
            put(routeK_[i]->getText());
        }
        return s;
    }
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
