// heddle-host — the standalone live JUCE VST3 chain host.
//
//   heddle-host <chain.json> [--audio null|system|ring] [--device NAME]
//             [--ring PREFIX] [--osc-port N] [--duration SECONDS]
//             [--gui] [--version]
//
// Loads the chain described by the config JSON, instantiates the plugins
// via JuceBackend, and runs them live:
//
//   * --audio system (default): a real output device via AudioDeviceManager;
//     the JUCE audio callback drives the chain at the device's rate/block.
//   * --audio null: no audio device is touched; a software clock thread
//     drives the chain at the config's sample rate / block size. Used for
//     headless (CI) testing and OSC-only use.
//   * --audio ring: no audio device is touched; instead the host owns two
//     named shared-memory audio rings (PREFIX.in / PREFIX.out, default
//     heddle.bridge.in / heddle.bridge.out) and a pump thread. The HeddleBridge
//     VST3 (or the bridge round-trip test) writes interleaved float32 frames
//     into PREFIX.in and reads the processed audio back from PREFIX.out. The
//     pump paces the chain at the config's sample rate / block size, exactly
//     one ring block at a time. This is the audio path the DAW-facing bridge
//     VST3 uses.
//
// OSC control protocol (see docs/OSC.md; the same framing core/src/osc.cpp
// implements for the offline tooling — every message is carried with a
// 4-byte big-endian length header, which the core's OscUdp adds/strips):
//
//   /chain/query                           -> /reply/chain (one string; one
//                                            line per slot:
//                                            "index<TAB>slotId<TAB>path<TAB>enabled")
//   /chain/<slot>/<param>  (no args)       -> /reply/<slot>/<param> ... (one
//                                            string line per param:
//                                            "id<TAB>title<TAB>norm<TAB>min<TAB>max<TAB>steps<TAB>kind")
//   /chain/<slot>/<param>  (float)         set param by name
//                                            -> /reply/<slot>/<param> (float ack)
//   /chain/<slot>/<param>  (int id, float) set param by id
//                                            -> /reply/<slot>/<param> (float ack)
//   /chain/<slot>/reset                    reset one slot to plugin defaults
//                                            -> /status (string)
//   /chain/reset                           reset all slots -> /status
//   /chain/apply    (string path)          hot-(re)load a config file
//   /chain/config   (string json)          hot-(re)load config from JSON text
//   /chain/status                          -> /reply/status (host status line)
//   /chain/quit                            -> /reply/status "quitting" + exit 0
//
//   /change/<slot>/<param>                 push: the plugin changed a param itself
//
// Threading model
// ---------------
// The audio side (device callback, or the null/ring pump thread) NEVER
// waits or skips on message-thread activity. Parameter changes are
// lock-free end to end: an OSC/GUI set calls the backend's
// setValueNotifyingHost from the message thread, which JUCE's VST3 host
// wrapper turns into atomic cached-value stores + a thread-safe
// parameter-dispatcher queue; the audio callback drains that queue into
// VST3 IParameterChanges at the top of each process() and the plugin
// latches the values inside its own process context. No shared lock
// exists on the param path — the same model every DAW host uses, so a
// param write mid-block can never stall, skip, or silence an audio
// block. Continuous param adjustment costs the audio path ~0
// (measured: see docs/PERFORMANCE.md).
//
// The ONE cross-thread meeting point is structural change (hot-apply:
// config swap / plugin (re)load, which destroys/recreates plugin
// instances). For those, the message thread calls quiesceAudio() — it
// spins until the in-flight audio block completes (bounded by one block,
// ~1-21 ms), then performs the swap while no callback can be running.
// The audio thread never waits for the message thread.
//
// Shared state after the rework:
//   * cfg_ / curve tables / chain model: written only on the message
//     thread, and only after quiesceAudio(); the audio callback reads
//     cfg_.slots concurrently (concurrent reads are fine; the only
//     writer quiesces first).
//   * backend_ param calls (setParams/paramValue/setValue): JUCE
//     thread-safe (atomics + dispatcher), callable from either thread.
//   * backend_.process / setProcessSpec: audio thread; setProcessSpec
//     only on spec change (first block / device change).
//   * chainFrame_: atomic, audio thread increments, message thread reads
//     (status line, curve evaluation).
//   * work buffers + ring scratch: audio thread only.
//
// An OSC set marks the param "user-locked": curve automation stops
// touching it until /chain/<slot>/reset or a hot-apply. Curve automation
// itself is evaluated on the message-thread timer, not in the audio
// callback: the interval is dynamic at 2*sampleRate/blockSize Hz (2 ticks
// per block, 5 ms floor to protect the OSC poll), and a param is only ever
// pushed while its curve still has an upcoming keyframe. This keeps the
// audio callback a pure DSP consumer and keeps hundreds of plugin controls
// essentially free.
//
// Headless pattern: plain main() + juce::initialiseJuce_GUI() — the same
// shape heddle_smoke / heddle-render already build with in CI (no
// JUCEApplication, no window). Timers are serviced by the
// MessageManager dispatch loop in run().
//
// --gui (default OFF)
// -------------------
// Opens a JUCE window listing every slot and its params as sliders. The
// sliders set params through the same path as the OSC set (Chain::setParam,
// user-locked against curve automation); the window tracks plugin-side
// changes via the same 5 ms refreshModel() poll. Off by default: headless
// OSC-only is the normal mode; the window is for interactive use.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_core/juce_core.h>

#include "JuceBackend.hpp"
#include "InBlockCurve.hpp"
#include "RawVst3.hpp"
#include "heddle/chain.hpp"
#include "heddle/config.hpp"
#include "heddle/osc.hpp"
#include "heddle/osc_io.hpp"
#include "heddle/ring.hpp"

// Stamped by CMake (configure_file -> heddle_version.hpp on this target's
// include path), same as heddle_smoke / heddle-render.
#include "heddle_version.hpp"
#ifndef HEDDLE_VERSION_STRING
#define HEDDLE_VERSION_STRING "unknown"
#endif

using heddle::Chain;
using heddle::ChainConfig;
using heddle::OscMsg;
using heddle::OscUdp;
using heddle::Param;
using heddle::ParamSet;
using heddle::ParamSpec;
using heddle::SlotConfig;
namespace osc_addr = heddle::osc_addr;

namespace {

void logInfo(const std::string& msg) {
    std::fprintf(stderr, "[heddle-host] %s\n", msg.c_str());
    std::fflush(stderr);
}

void logErr(const std::string& msg) {
    std::fprintf(stderr, "[heddle-host] ERROR: %s\n", msg.c_str());
    std::fflush(stderr);
}

std::string fmtF(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

// Software clock for --audio null: paces the NULL form of
// audioDeviceIOCallbackWithContext (numSamples == 0, null buffers) at the
// config's rate/block. The callback itself decides the block size from its
// config when numSamples <= 0.
class NullClockThread : public juce::Thread {
public:
    NullClockThread() : juce::Thread("heddle-null-clock") {}

    // Set before startThread(); cb must outlive the thread (the host owns
    // both and stops the thread in stopAll()).
    void setPace(juce::AudioIODeviceCallback* cb, int sampleRate, int blockSize) {
        cb_ = cb;
        blockMs_ = std::max(
            1, static_cast<int>(std::ceil(static_cast<double>(blockSize) * 1000.0 /
                                          std::max(1, sampleRate))));
    }

    void stopClock() { running_ = false; }

private:
    void run() override {
        while (running_) {
            const int64_t target = juce::Time::getMillisecondCounter() + blockMs_;
            if (cb_ != nullptr)
                cb_->audioDeviceIOCallbackWithContext(nullptr, 0, nullptr, 0, 0, {});
            const int64_t now = juce::Time::getMillisecondCounter();
            if (target > now)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(target - now));
        }
    }

    juce::AudioIODeviceCallback* cb_ = nullptr;
    int blockMs_ = 11;
    std::atomic<bool> running_{true};
};

// Audio-ring pump for --audio ring: paces the ring form of
// audioDeviceIOCallbackWithContext (numSamples == 0, null buffers) at the
// config's rate/block, but only when the producer actually has a full block
// of data in the input ring AND the consumer has room for a full block in
// the output ring. The callback itself decides the block size from its
// config when numSamples <= 0 (identical to the null-clock tick); the
// ring-mode run() override in HeddleHost feeds the real audio in/out through
// the rings. When the input ring is empty the callback is NOT invoked (no
// chain processing, no output written), so the consumer side (the DAW-side
// bridge or the round-trip test) simply sees no new output until input
// arrives.
//
// Backpressure (the output-space check): the callback's ringOut write is
// best-effort (a full ring returns 0 and DROPS the block). Checking
// spaceAvailable() >= frames_ before every tick makes the ring pair a true
// lossless FIFO in both directions: a slow consumer stalls the pump
// (backpressure) instead of silently eating output samples. Both rings are
// SPSC, so these two cursor reads are lock-free and race-safe.
class AudioRingPumpThread : public juce::Thread {
public:
    AudioRingPumpThread() : juce::Thread("heddle-ring-pump") {}

    // Set before startThread(); the rings are owned by the host and outlive
    // the thread (the host stops the thread in stopAll() before closing the
    // rings).
    void setPace(juce::AudioIODeviceCallback* cb, heddle::AudioRing* inRing,
                 heddle::AudioRing* outRing, int sampleRate, int blockSize) {
        cb_ = cb;
        inRing_ = inRing;
        outRing_ = outRing;
        const int bs = blockSize > 0 ? blockSize : 512;
        frames_ = bs;
        // Real-time pacing (exactly like NullClockThread): one block's worth of
        // audio takes bs*1000/sampleRate ms. A fixed 1 ms poll instead would
        // pump ~(1000*bs/sr) blocks/s — for the default 1024@48k that is 1000
        // blocks/s vs 46.9/s in real time, i.e. the pump runs the chain 21x
        // faster than real time, the input ring backlogs to full, and the
        // output offset drifts. Pacing at the block's real-time duration keeps
        // input write rate ~= pump rate, the pipeline depth stable, and the
        // round-trip offset measurable.
        blockNs_ = (long long)(std::ceil(
            (double)bs * 1e9 / (double)std::max(1, sampleRate)));
    }

    void stopPump() { running_ = false; }

private:
    void run() override {
        auto target = std::chrono::steady_clock::now();
        while (running_) {
            const bool busy = (cb_ != nullptr && inRing_ != nullptr && outRing_ != nullptr &&
                               inRing_->dataAvailable() >= frames_ &&
                               outRing_->spaceAvailable() >= frames_);
            if (busy) {
                cb_->audioDeviceIOCallbackWithContext(nullptr, 0, nullptr, 0, 0, {});
                // Advance the real-time schedule by one block AFTER processing
                // (never before, and only when a block actually went through —
                // otherwise a backpressure stall would eat time and the pump
                // would run slower than real time).
                target += std::chrono::nanoseconds(blockNs_);
            } else {
                // Blocked on input/output: re-anchor so a long stall doesn't
                // accumulate into a catch-up burst.
                target = std::chrono::steady_clock::now();
            }
            // Absolute-schedule sleep: sleep_for(pollMs_) would drift on
            // Windows (15 ms timer granularity) and slowly desync input/pump,
            // corrupting the round-trip sample accounting. Sleep until near the
            // target, then busy-spin the last ~2 ms for sub-millisecond
            // accuracy (the window is tiny, so the CPU cost is negligible).
            while (running_) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= target) break;
                const auto rem = std::chrono::duration_cast<std::chrono::nanoseconds>(target - now).count();
                if (rem > 2000000)
                    std::this_thread::sleep_for(std::chrono::nanoseconds(rem - 2000000));
            }
        }
    }

    juce::AudioIODeviceCallback* cb_ = nullptr;
    heddle::AudioRing* inRing_ = nullptr;
    heddle::AudioRing* outRing_ = nullptr;
    int frames_ = 512;
    long long blockNs_ = 21333;  // 1024 @ 48k
    std::atomic<bool> running_{true};
};

}  // namespace

class HeddleHostWindow;  // --gui window; defined after HeddleHost (its content
                       // callbacks need the complete HeddleHost type)

// ----------------------------------------------------------------------------
// HeddleHost
// ----------------------------------------------------------------------------
class HeddleHost : public juce::AudioIODeviceCallback,
                 public juce::Timer {
public:
    HeddleHost() = default;
    ~HeddleHost() override { stopAll(); }

    int run(int argc, char** argv) {
        if (!parseArgs(argc, argv))
            return 1;
        if (args_.printVersion)
            return 0;

        // The main thread becomes the JUCE message thread; all JuceBackend
        // (plugin) calls below happen on it.
        juce::initialiseJuce_GUI();

        std::string err;
        if (!heddle::config::parseFile(args_.configPath, cfg_, &err)) {
            logErr("config: " + err);
            return 1;
        }
        if (cfg_.slots.empty()) {
            logErr("config has no slots");
            return 1;
        }
        // In-block curve scheduling mode (message-thread-only writes, before
        // the audio thread starts, so no synchronisation is needed).
        // Resolution: an explicit --substep N wins; otherwise the config's
        // curve_resolution. 0 = whole-queue in-block path: one raw process()
        // call per block carrying a two-point IParamValueQueue per active param
        // (the plugin interpolates per-sample -> smooth chord). N > 0 =
        // oversample: the block is split into N-sample sub-slices, each latching
        // one control point (a finer staircase that converges to the chord).
        curveSubstep_ =
            args_.substep >= 0 ? args_.substep : cfg_.curveResolution;
        inblock_ = args_.inblock;
        dbgWin_ = (std::getenv("HEDDLE_DBG_WIN") != nullptr);
        if (dbgWin_) {
            const char* dp = std::getenv("HEDDLE_DBG_FILE");
            dbgWinFile_ = std::fopen(dp ? dp : "dbg_win.txt", "w");
            setvbuf(dbgWinFile_, nullptr, _IONBF, 0);  // unbuffered
        }
        if (!chain.load(cfg_, &err)) {
            logErr("chain load failed: " + err);
            return 1;
        }
        // Build the in-block curve scheduler's fixed-size entry table (one per
        // enabled slot's param) from the now-loaded chain. Message thread,
        // before the audio side starts, so the vector's layout is stable for
        // the audio thread's later lock-free iteration.
        if (inblock_) initCurveSlots();
        buildAutomationTables();
        // Plugin-internal changes (detected by refreshModel()) are pushed to
        // the last OSC peer as /change/<slot>/<param>.
        chain.onChange([this](int slotIndex, const Param& p) {
            osc_.sendTo(OscMsg::make(osc_addr::makeChangeAddr(slotIndex, p.title), p.value),
                        lastSrc_);
        });

        // Open the GUI window BEFORE starting the audio thread. The chain
        // model is message-thread-only (the audio callback no longer reads
        // it), so no lock-ordering hazard remains; the ordering is kept
        // because the window's initial snapshot is cheapest before the
        // audio side starts mutating plugin state.
        if (args_.gui)
            guiStart();  // open the param-control window (message thread)

        if (args_.audioMode == "system") {
            if (!initAudioDevice(&err)) {
                logErr(err);
                return 1;
            }
        } else if (args_.audioMode == "ring") {
            // Create the two shared-memory rings and start the pump thread.
            // The bridge (or the round-trip test) attaches via AudioRing::open.
            const int nch = 2;  // fixed stereo bridge bus
            const int cap = (cfg_.blockSize > 0 ? cfg_.blockSize : 512) * 16;
            ringInName_ = args_.ringPrefix + ".in";
            ringOutName_ = args_.ringPrefix + ".out";
            if (heddle::AudioRing::exists(ringInName_))
                heddle::AudioRing::remove(ringInName_);
            if (heddle::AudioRing::exists(ringOutName_))
                heddle::AudioRing::remove(ringOutName_);
            ringIn_ = heddle::AudioRing::create(ringInName_, cap, nch, cfg_.sampleRate, &err);
            if (!ringIn_.valid()) {
                logErr("ring create " + ringInName_ + ": " + err);
                return 1;
            }
            ringOut_ = heddle::AudioRing::create(ringOutName_, cap, nch, cfg_.sampleRate, &err);
            if (!ringOut_.valid()) {
                logErr("ring create " + ringOutName_ + ": " + err);
                ringIn_.close();
                return 1;
            }
            logInfo("audio=ring (shared memory: " + ringInName_ + " / " + ringOutName_ +
                    ", " + std::to_string(nch) + " ch, " + std::to_string(cfg_.sampleRate) +
                    " Hz, " + std::to_string(cfg_.blockSize) + "-sample blocks, capacity " +
                    std::to_string(cap) + " frames); no audio device opened");
            ringPump_ = std::make_unique<AudioRingPumpThread>();
            ringPump_->setPace(this, &ringIn_, &ringOut_, cfg_.sampleRate, cfg_.blockSize);
            ringPump_->startThread();
        } else {  // null
            logInfo("audio=null (software clock @ " + std::to_string(cfg_.sampleRate) +
                    " Hz, " + std::to_string(cfg_.blockSize) +
                    "-sample blocks); no audio device opened");
            clock_ = std::make_unique<NullClockThread>();
            clock_->setPace(this, cfg_.sampleRate, cfg_.blockSize);
            clock_->startThread();
        }

        if (!setupOsc()) {
            logErr("OSC bind failed: " + oscErr_);
            return 1;
        }

        logInfo("ready, v" HEDDLE_VERSION_STRING ", " + std::to_string(chain.numSlots()) +
                " slot(s), OSC UDP " + bindHost_ + ":" + std::to_string(osc_.localPort()) +
                (args_.durationSec > 0.0 ? ", stopping in " + fmtF(args_.durationSec) + " s"
                                         : ""));

        startTimer(5);  // 5 ms OSC poll from the message thread
        updateAutomationPeriod();  // scale automation cadence to the block size

        const int64_t startMs = juce::Time::getMillisecondCounter();
        const int64_t endMs =
            (args_.durationSec > 0.0)
                ? startMs + static_cast<int64_t>(args_.durationSec * 1000.0)
                : -1;
        // Pump the JUCE message loop: services the OSC Timer and any async
        // JUCE updates. The loop exits on /chain/quit or --duration.
        while (!quit_.load()) {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
            if (endMs > 0 && juce::Time::getMillisecondCounter() >= endMs)
                break;
        }

        stopAll();
        return 0;
    }

    // ---- juce::AudioIODeviceCallback --------------------------------------
    // numSamples <= 0 (and null buffers) is the null-clock tick: process one
    // config-size block. numSamples > 0 is a real device callback.
    void audioDeviceIOCallbackWithContext(const float* const* inData, int numIns,
                                          float* const* outData, int numOuts,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& ctx) override {
        (void)ctx;  // only carries hostTimeNs; rate/block come from the device
        const bool nullTick = (numSamples <= 0);
        // --audio ring: the pump thread drives the same null-form tick, but
        // the real audio flows through the shared-memory rings (ringIn_ /
        // ringOut_ are valid only in ring mode).
        const bool ringTick = nullTick && ringIn_.valid() && ringOut_.valid();
        const int frames =
            nullTick ? (cfg_.blockSize > 0 ? cfg_.blockSize : 512) : numSamples;
        const double rate =
            nullTick ? (cfg_.sampleRate > 0 ? static_cast<double>(cfg_.sampleRate) : 44100.0)
                     : (devRate_ > 0.0 ? devRate_ : static_cast<double>(cfg_.sampleRate));

        // The audio side never waits or skips (the old try-lock + silence
        // policy is gone — see the threading-model comment at the top of
        // this file). Parameter changes are lock-free end to end, so there
        // is nothing to contend on. blockInFlight_ only serialises
        // structural swaps (quiesceAudio) — it is never waited on by the
        // audio side.
        blockInFlight_.store(true, std::memory_order_seq_cst);

        if (specRate_ != rate || specBlock_ != frames) {
            specRate_ = rate;
            specBlock_ = frames;
            backend_.setProcessSpec(rate, frames);
        }

        // In ring mode the real input comes from ringIn_ (the pump thread is
        // its sole consumer — SPSC). De-interleave a full block into a planar
        // scratch and feed it to runBlockLocked exactly like device input;
        // runBlockLocked() clears workIn_ itself, so the audio must arrive via
        // inData, not by pre-filling workIn_.
        const float* const* runIn = inData;
        int runIns = numIns;
        if (ringTick) {
            const size_t nch = (size_t)ringIn_.numChannels();
            if (nch > 0) {
                // Read interleaved from the ring, de-interleave into planar
                // scratch, and hand the planar pointers to runBlockLocked.
                const size_t n = (size_t)frames;
                ringInInter_.resize(nch * n);
                const size_t got = ringIn_.read(ringInInter_.data(), n);
                if (got < n)
                    std::fill(ringInInter_.begin() + got * nch, ringInInter_.end(), 0.f);
                ringInBuf_.resize(nch * n);
                ringInPtrs_.resize(nch);
                for (size_t f = 0; f < got; ++f) {
                    for (size_t c = 0; c < nch; ++c)
                        ringInBuf_[c * n + f] = ringInInter_[f * nch + c];
                }
                for (size_t f = got; f < n; ++f)
                    for (size_t c = 0; c < nch; ++c)
                        ringInBuf_[c * n + f] = 0.f;
                for (size_t c = 0; c < nch; ++c)
                    ringInPtrs_[c] = ringInBuf_.data() + c * n;
                runIn = ringInPtrs_.data();
                runIns = (int)nch;
            }
        }

        runBlock(runIn, runIns, frames);

        if (outData != nullptr) {
            const int width = std::max(1, numOuts);
            for (int c = 0; c < width; ++c) {
                if (outData[c] == nullptr)
                    continue;
                const float* src = (c < workOut_.getNumChannels())
                                       ? workOut_.getReadPointer(c)
                                       : nullptr;
                if (src != nullptr)
                    std::memcpy(outData[c], src, static_cast<size_t>(frames) * sizeof(float));
                else
                    std::fill(outData[c], outData[c] + frames, 0.f);
            }
        }

        if (ringTick) {
            // Interleave workOut_ into ringOut_ (the pump thread is its sole
            // producer — SPSC). Channels beyond the ring width are dropped.
            const size_t nch = (size_t)ringOut_.numChannels();
            if (nch > 0) {
                ringOutBuf_.resize(nch * (size_t)frames);
                for (int c = 0; c < (int)nch; ++c)
                    for (int f = 0; f < frames; ++f)
                        ringOutBuf_[(size_t)f * nch + c] =
                            (c < workOut_.getNumChannels())
                                ? workOut_.getReadPointer(c)[f] : 0.f;
                ringOut_.write(ringOutBuf_.data(), (size_t)frames);
            }
        }

        blockInFlight_.store(false, std::memory_order_seq_cst);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        if (device != nullptr) {
            devRate_ = device->getCurrentSampleRate();
            devBlock_ = device->getCurrentBufferSizeSamples();
        }
        running_ = true;
        logInfo("audio device started (" + std::to_string((int)devRate_) + " Hz, " +
                std::to_string(devBlock_) + "-sample blocks)");
        updateAutomationPeriod();  // the device's real block size wins
    }

    void audioDeviceStopped() override {
        running_ = false;
        logInfo("audio device stopped");
    }

    void audioDeviceError(const juce::String& message) override {
        logErr("audio device error: " + message.toStdString());
    }

    // ---- juce::Timer (OSC poll, message thread) ----------------------------
    void timerCallback() override {
        OscMsg msg;
        OscUdp::SourceAddr src;
        for (;;) {
            if (!osc_.poll(msg, &src))
                break;
            lastSrc_ = src;
            handleOsc(msg, src);
        }
        // Curve automation at its dynamic period (2x per audio block, 5 ms
        // floor): gated inside the constant 5 ms OSC timer so a long block
        // period never slows OSC polling.
        const int64_t nowMs = juce::Time::getMillisecondCounter();
        if (lastAutoMs_ == 0) {
            lastAutoMs_ = nowMs;
        } else if (nowMs - lastAutoMs_ >= automationPeriodMs_) {
            lastAutoMs_ = nowMs;
            curveAutomationTick();
        }
        refreshModel();
        guiPull();
    }

    // The --gui content reads the chain model directly (message thread; the
    // model is message-thread-only after the lock-free rework); see
    // ChainGuiContent below.
    friend class ChainGuiContent;

    // ---- --gui entry points (message thread) --------------------------------
    // hostGuiSet: set a param by name the same way the OSC set path does and
    // mark it user-locked (manual control outranks curve automation).
    bool hostGuiSet(int slotIndex, const std::string& name, double norm);
    // requestQuit: window close (same as /chain/quit).
    void requestQuit();

private:
    // ---- args ---------------------------------------------------------------
    struct Args {
        std::string configPath;
        std::string audioMode = "system";
        std::string deviceName;
        std::string ringPrefix = "heddle.bridge";  // --ring: PREFIX.in / PREFIX.out
        int oscPort = 0;            // 0 = not given on the CLI
        bool printVersion = false;
        double durationSec = 0.0;
        bool gui = false;           // --gui: open the param-control window
        bool inblock = true;        // --no-inblock: legacy immediate latch
        int substep = -1;           // --substep: in-block curve segment size;
                                    // -1 = use config curve_resolution (0 =
                                    // one control point per block = default)
    };

    bool parseArgs(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto need = [&](const char* flag) -> const char* {
                if (i + 1 >= argc) {
                    logErr(std::string(flag) + " needs a value");
                    return nullptr;
                }
                return argv[++i];
            };
            if (a == "--version" || a == "-V") {
                args_.printVersion = true;
            } else if (a == "--audio") {
                const char* v = need("--audio");
                if (v == nullptr) return false;
                args_.audioMode = v;
            } else if (a == "--device") {
                const char* v = need("--device");
                if (v == nullptr) return false;
                args_.deviceName = v;
            } else if (a == "--ring") {
                const char* v = need("--ring");
                if (v == nullptr) return false;
                args_.ringPrefix = v;
            } else if (a == "--osc-port") {
                const char* v = need("--osc-port");
                if (v == nullptr) return false;
                args_.oscPort = std::atoi(v);
            } else if (a == "--duration") {
                const char* v = need("--duration");
                if (v == nullptr) return false;
                args_.durationSec = std::atof(v);
            } else if (a == "--gui") {
                args_.gui = true;
            } else if (a == "--no-inblock") {
                // Legacy behaviour: an OSC set latches the plugin immediately
                // (one value per set, no in-block curve). Used as the
                // baseline in the rate sweep so the curve's benefit is
                // measurable.
                args_.inblock = false;
            } else if (a == "--substep") {
                const char* v = need("--substep");
                if (v == nullptr) return false;
                const int s = std::atoi(v);
                // 0 = whole-queue in-block path (one raw process()/block,
                // 2-point queue per active param, plugin interpolates);
                // N > 0 = N-sample oversampled sub-slices.
                if (s < 0) { logErr("--substep must be >= 0"); return false; }
                args_.substep = s;
            } else if (a == "--help" || a == "-h") {
                usage(argv[0]);
                args_.printVersion = true;  // handled in run(): exit 0
            } else if (!a.empty() && a[0] == '-') {
                logErr("unknown option: " + a);
                usage(argv[0]);
                return false;
            } else if (args_.configPath.empty()) {
                args_.configPath = a;
            } else {
                logErr("unexpected argument: " + a);
                usage(argv[0]);
                return false;
            }
        }
        if (args_.printVersion) {
            std::printf("heddle-host %s\n", HEDDLE_VERSION_STRING);
            return true;
        }
        if (args_.configPath.empty()) {
            logErr("missing config path");
            usage(argv[0]);
            return false;
        }
        if (args_.audioMode != "system" && args_.audioMode != "null" &&
            args_.audioMode != "ring") {
            logErr("unknown --audio '" + args_.audioMode +
                   "' (want 'system', 'null', or 'ring')");
            return false;
        }
        return true;
    }

    static void usage(const char* prog) {
        std::fprintf(stderr,
            "usage: %s <chain.json> [--audio null|system|ring] [--device NAME]\n"
            "       [--ring PREFIX] [--osc-port N] [--duration SECONDS] [--gui]\n"
            "       [--no-inblock] [--substep N] [--version]\n"
            "  Live VST3 chain host (JUCE). OSC control over UDP; the config's\n"
            "  osc.port/bind_host are the defaults, --osc-port overrides the port.\n"
            "  --audio ring serves the HeddleBridge VST3 over the shared-memory rings\n"
            "  PREFIX.in / PREFIX.out (default heddle.bridge.in / heddle.bridge.out).\n"
            "  --gui opens a window with per-slot param sliders (default off,\n"
            "  headless).\n"
            "  In-block curve scheduling is ON by default: every OSC set is a\n"
            "  sample-accurate waypoint (2-block lookahead). --no-inblock restores\n"
            "  the legacy immediate-latch behavior. --substep N sets the in-block\n"
            "  resolution: 0 = whole queue (default; one raw process() per block,\n"
            "  two control points per active param, plugin interpolates); N > 0 =\n"
            "  oversample (N-sample sub-slices). Omitting it uses the config's\n"
            "  curve_resolution.\n",
            prog);
    }

    // ---- audio setup ---------------------------------------------------------
    bool initAudioDevice(std::string* err) {
        const juce::String initErr = adm_.initialise(
            0, 2, nullptr, true,
            juce::String(args_.deviceName.c_str()), nullptr);
        if (!initErr.isEmpty()) {
            if (err) *err = "audio device init failed: " + initErr.toStdString();
            return false;
        }
        if (adm_.getCurrentAudioDevice() == nullptr) {
            if (err) *err = "no audio output device available";
            return false;
        }
        adm_.addAudioCallback(this);
        if (adm_.getCurrentAudioDevice() == nullptr) {
            if (err) *err = "audio device would not start";
            return false;
        }
        logInfo("audio device: " + adm_.getCurrentAudioDevice()->getName().toStdString());
        return true;
    }

    // OSC config is the source of truth for port/bind; --osc-port overrides
    // the port. A port of 0 after that falls back to 9001.
    bool setupOsc() {
        const int port =
            (args_.oscPort > 0) ? args_.oscPort
                                : ((cfg_.osc.port > 0) ? cfg_.osc.port : 9001);
        bindHost_ = cfg_.osc.bindHost.empty() ? std::string("127.0.0.1") : cfg_.osc.bindHost;
        if (!osc_.bind(bindHost_, port, &oscErr_))
            return false;
        return true;
    }

    // ---- per-block processing (audio thread; no locks) ----------------------
    // Pure DSP consumer. Param values are normally latched by the plugins
    // themselves via VST3 IParameterChanges (JUCE drains the atomic cached-value
    // queue inside each process()). In in-block mode, instead, the host pushes a
    // chord value for every active param at the start of each sub-block and runs
    // that sub-block, so a block becomes a fine staircase converging on the true
    // in-block curve (see the curve scheduler below).
    void runBlock(const float* const* inData, int numIns, int frames) {
        const int width = std::max(2, backend_.busWidth());
        if ((int)workIn_.getNumChannels() < width || workIn_.getNumSamples() < frames) {
            workIn_.setSize(width, frames);
            workA_.setSize(width, frames);
            workB_.setSize(width, frames);
            workOut_.setSize(width, frames);
        }
        workIn_.clear();
        workA_.clear();
        workB_.clear();
        workOut_.clear();

        // Chain input: device input when present (system mode), else silence.
        if (inData != nullptr)
            for (int c = 0; c < std::min(numIns, width); ++c)
                if (inData[c] != nullptr)
                    std::memcpy(workIn_.getWritePointer(c), inData[c],
                                static_cast<size_t>(frames) * sizeof(float));

        // In-block curve scheduling: drain the waypoint rings for THIS block and
        // rebuild the per-slot working set. runBlock is the sole audio entry
        // point, so no lock. With no active curve, slotsCurve_ is empty and we
        // take the fast single-pass path (identical to the legacy behaviour).
        const uint32_t curBlock = audioBlock_.fetch_add(1, std::memory_order_relaxed);
        if (inblock_) prepareCurvesForBlock(curBlock);
        const bool haveCurve = !slotsCurve_.empty();

        const uint32_t S = static_cast<uint32_t>(frames);
        // Fast path: no curve active -> one process() per slot, full block.
        if (!haveCurve) {
            juce::AudioBuffer<float>* cur = &workIn_;
            for (const auto& sc : cfg_.slots) {
                if (!sc.enabled) continue;
                juce::AudioBuffer<float>* next = (cur == &workA_) ? &workB_ : &workA_;
                const int processed =
                    backend_.process(sc.id, cur->getWritePointer(0, 0),
                                     next->getWritePointer(0, 0), width, frames);
                if (processed != frames) {
                    logErr("slot '" + sc.id + "' failed to process " + std::to_string(frames) +
                           " frames (got " + std::to_string(processed) + ")");
                    cur = nullptr;
                    break;
                }
                cur = next;
            }
            if (cur != nullptr) {
                const int n = std::min(workOut_.getNumChannels(), cur->getNumChannels());
                for (int c = 0; c < n; ++c)
                    workOut_.copyFrom(c, 0, *cur, c, 0, frames);
            }
            chainFrame_.fetch_add(frames, std::memory_order_relaxed);
            return;
        }

        // ---- curve_resolution == 0 : whole-queue in-block path -------------
        // Instead of latching one value and stepping the block in sub-slices
        // (the staircase), hand each slot a single IParameterChanges holding a
        // two-point queue per active param (start @ offset 0, end @ offset
        // S-1) and drive its raw Vst::IAudioProcessor once for the whole block.
        // The plugin linearly interpolates between the two queue points
        // per-sample, so the rendered parameter follows the true chord -- smooth,
        // not a staircase. This is the DEFAULT in-block behaviour. A slot whose
        // plugin is not a VST3 (no raw IAudioProcessor) latches the block-start
        // chord and runs the block through the JUCE path (its best we can do).
        if (curveSubstep_ == 0) {
            std::vector<float*> inPtrs(width, nullptr);
            std::vector<float*> outPtrs(width, nullptr);
            heddle::rawv::RawParamChanges changes;
            struct Pt { uint32_t paramId; size_t idx; float v1; };
            std::vector<Pt> pts;
            juce::AudioBuffer<float>* cur = &workIn_;
            for (size_t s = 0; s < cfg_.slots.size(); ++s) {
                const auto& sc = cfg_.slots[s];
                if (!sc.enabled) continue;
                juce::AudioBuffer<float>* next = (cur == &workA_) ? &workB_ : &workA_;
                for (int c = 0; c < width; ++c) {
                    inPtrs[c]  = cur->getWritePointer(c);
                    outPtrs[c] = next->getWritePointer(c);
                }
                // Two-point queue for THIS slot's active params only.
                changes.clear();
                pts.clear();
                for (const auto& scv : slotsCurve_) {
                    if (scv.first != (int) s) continue;
                    for (const auto& c : scv.second) {
                        CurveEntry& e = curveSlots_[c.idx];
                        const float v0 = heddle_InBlockCurve::chordAt(
                            e.window.data(), e.window.size(), curBlock, S, 0, e.carry);
                        const float v1 = heddle_InBlockCurve::chordAt(
                            e.window.data(), e.window.size(), curBlock, S, S - 1, e.carry);
                        changes.append(c.paramId, (double) v0, (double) v1, (int) S);
                        pts.push_back({c.paramId, c.idx, v1});
                        if (dbgWin_ && dbgWinFile_)
                            std::fprintf(dbgWinFile_,
                                         "SEND blk=%u p=%u v0=%.6f v1=%.6f\n",
                                         curBlock, c.paramId, v0, v1);
                    }
                }
                const int processed = backend_.rawProcess(
                    sc.id, inPtrs.data(), outPtrs.data(), width,
                    static_cast<int>(S),
                    changes.count() > 0 ? static_cast<void*>(&changes) : nullptr);
                if (processed == static_cast<int>(S)) {
                    for (const Pt& p : pts)
                        curveSlots_[p.idx].carry = p.v1;  // plugin latches v1
                } else {
                    // No raw interface (non-VST3) or raw call failed: latch the
                    // block-start chord and run the whole block via JUCE.
                    pushCurveAtSlot((int) s, curBlock, S, 0);
                    const int p2 = backend_.processChannels(
                        sc.id, inPtrs.data(), outPtrs.data(), width,
                        static_cast<int>(S));
                    if (p2 != static_cast<int>(S)) {
                        logErr("slot '" + sc.id + "' failed to process " +
                               std::to_string(S) + " frames (got " +
                               std::to_string(p2) + ")");
                        return;
                    }
                }
                cur = next;
            }
            // Output: after the last slot, `cur` holds the fully-rendered block.
            {
                juce::AudioBuffer<float>* out = &workIn_;
                for (const auto& sc : cfg_.slots)
                    if (sc.enabled) out = (out == &workA_) ? &workB_ : &workA_;
                const int n = std::min(workOut_.getNumChannels(), out->getNumChannels());
                for (int c = 0; c < n; ++c)
                    workOut_.copyFrom(c, 0, *out, c, 0, frames);
            }
            chainFrame_.fetch_add(frames, std::memory_order_relaxed);
            return;
        }

        // ---- curve_resolution > 0 : sub-block staircase path ---------------
        // Segment the block into sub-blocks of `seg` samples; at each sub-block,
        // push every active param for each enabled slot at the chord value for
        // its first sample, then run the slot ping-pong for that slice via
        // processChannels (explicit per-channel pointers at offset `done`, since
        // the work buffers' channel stride is the full block, not `seg`).
        const uint32_t seg =
            std::max<uint32_t>(1, static_cast<uint32_t>(curveSubstep_));
        std::vector<const float*> inPtrs(width, nullptr);
        std::vector<float*> outPtrs(width, nullptr);
        uint32_t done = 0;
        while (done < S) {
            const uint32_t n = std::min(seg, S - done);
            // Push the chord for this sub-block before processing it.
            for (int s = 0; s < (int)cfg_.slots.size(); ++s)
                if (cfg_.slots[s].enabled)
                    pushCurveAtSlot(s, curBlock, S, done);
            // Ping-pong this slice.
            juce::AudioBuffer<float>* cur = &workIn_;
            for (const auto& sc : cfg_.slots) {
                if (!sc.enabled) continue;
                juce::AudioBuffer<float>* next = (cur == &workA_) ? &workB_ : &workA_;
                for (int c = 0; c < width; ++c) {
                    inPtrs[c] = cur->getWritePointer(c);   // + done below
                    outPtrs[c] = next->getWritePointer(c);
                }
                for (int c = 0; c < width; ++c) inPtrs[c] += done;
                for (int c = 0; c < width; ++c) outPtrs[c] += done;
                const int processed =
                    backend_.processChannels(sc.id, inPtrs.data(), outPtrs.data(),
                                             width, static_cast<int>(n));
                if (processed != static_cast<int>(n)) {
                    logErr("slot '" + sc.id + "' failed to process sub-block of " +
                           std::to_string(n) + " frames (got " + std::to_string(processed) + ")");
                    return;  // abort the block; caller treats it as done
                }
                cur = next;
            }
            done += n;
        }
        // Output: after the last segment, `cur` holds the fully-rendered block
        // (each segment wrote its disjoint slice). Copy all of it to workOut_.
        {
            juce::AudioBuffer<float>* cur = &workIn_;
            for (const auto& sc : cfg_.slots)
                if (sc.enabled) cur = (cur == &workA_) ? &workB_ : &workA_;
            const int n = std::min(workOut_.getNumChannels(), cur->getNumChannels());
            for (int c = 0; c < n; ++c)
                workOut_.copyFrom(c, 0, *cur, c, 0, frames);
        }
        chainFrame_.fetch_add(frames, std::memory_order_relaxed);
    }

    // ---- curve automation (message thread, dynamic timer) -------------------
    // Moved out of the audio callback: it only touches the chain model, the
    // user-lock map, and backend_ param pushes (all JUCE thread-safe). The
    // timer runs at 2 ticks per audio block (2*sampleRate/blockSize Hz,
    // floored at 5 ms to protect the OSC poll): one tick per block is the
    // minimum rate at which the plugin keeps up with a block-changing curve,
    // two gives margin, and at 48 kHz / 1024 that is ~94 Hz.
    //
    // Push discipline (plugins expose hundreds of controls): a param is
    // only pushed while its curve still has an UPKOMING keyframe. Static
    // params and finished curves cost exactly nothing; the work scales with
    // the number of curves currently in motion, not the plugin's parameter
    // count.
    void curveAutomationTick() {
        const int64_t frame = chainFrame_.load(std::memory_order_relaxed);
        for (const auto& kv : curveTables_) {
            const int slotIdx = kv.first;
            if (slotIdx < 0 || slotIdx >= (int)cfg_.slots.size()) continue;
            const SlotConfig& sc = cfg_.slots[slotIdx];
            if (!sc.enabled) continue;
            std::vector<std::pair<std::string, float>> changed;
            for (const auto& name : kv.second) {
                const ParamSpec* spec = sc.paramSpec(name);
                if (spec == nullptr) continue;
                // No keyframe after the playhead: the curve is finished, the
                // plugin already holds the last value, and nothing will move
                // it again. Skip the push entirely (and the bookkeeping).
                if (!spec->hasUpcoming(frame)) continue;
                const Param* pp = chain.slot(slotIdx) ? chain.slot(slotIdx)->findByName(name) : nullptr;
                // In in-block mode a param under an active in-block curve is
                // owned by the audio staircase (pushed per sub-block); the tick
                // must not race it. (In legacy mode isCurveActive is false.)
                if (pp != nullptr && isCurveActive(slotIdx, pp->id)) {
                    prevCurve_[std::make_pair(slotIdx, name)] =
                        Param::clamp01(spec->at(frame));
                    continue;
                }
                const float v = Param::clamp01(spec->at(frame));
                const auto key = std::make_pair(slotIdx, name);
                auto it = prevCurve_.find(key);
                if (it == prevCurve_.end() || std::fabs(v - it->second) > 1e-6f) {
                    const auto lk = userLocked_.find(key);
                    if (lk == userLocked_.end() || !lk->second)
                        changed.emplace_back(name, v);
                    prevCurve_[key] = v;
                }
            }
            if (!changed.empty())
                backend_.setParams(sc.id, changed);
        }
    }

    // Message thread: the automation cadence is 2 ticks per audio block at
    // the current sample rate (period = blockSize / (2*sampleRate) seconds;
    // 5 ms floor). It is computed as a PERIOD, not as the timer interval:
    // the JUCE timer keeps polling OSC at a constant 5 ms and
    // curveAutomationTick() is gated inside timerCallback() by this period,
    // so a large block (long period) never slows the OSC poll, and a tiny
    // block never starves it.
    void updateAutomationPeriod() {
        const double sr =
            (devRate_ > 0.0) ? devRate_ :
            (cfg_.sampleRate > 0 ? static_cast<double>(cfg_.sampleRate) : 48000.0);
        const int blk =
            (devBlock_ > 0) ? devBlock_ :
            (cfg_.blockSize > 0 ? cfg_.blockSize : 512);
        int periodMs = (int)std::lround(1000.0 * blk / (2.0 * sr));
        if (periodMs < 5) periodMs = 5;
        if (periodMs != automationPeriodMs_) {
            automationPeriodMs_ = periodMs;
            lastAutoMs_ = 0;  // re-arm the gate
        }
    }

    // ---- structural swap quiesce (message thread) ----------------------------
    // Waits until the in-flight audio block (if any) finishes, so the
    // message thread can swap cfg_ / plugin instances with no callback
    // running. Bounded by one block (~1-21 ms at 48 kHz; ~1 ms in
    // null/ring mode). The audio side NEVER waits for the message thread —
    // only this direction exists.
    void quiesceAudio() {
        while (blockInFlight_.load(std::memory_order_acquire))
            juce::Thread::yield();
    }

    // Map slot index -> param names that are curves (automation only touches
    // these; static params were pushed once at load/apply time).
    void buildAutomationTables() {
        curveTables_.clear();
        prevCurve_.clear();
        for (size_t s = 0; s < cfg_.slots.size(); ++s) {
            std::vector<std::string> names;
            for (const auto& kv : cfg_.slots[s].params)
                if (kv.second.isCurve())
                    names.push_back(kv.first);
            if (!names.empty())
                curveTables_[static_cast<int>(s)] = std::move(names);
        }
    }

    void clearOverrides() {
        userLocked_.clear();
        prevCurve_.clear();
    }

    // True if (slotIdx, paramId) currently has an active in-block curve. Used
    // by curveAutomationTick to defer to the audio staircase (which owns the
    // plugin value) instead of racing it from the message-thread tick.
    bool isCurveActive(int slotIdx, uint32_t paramId) const {
        if (!inblock_) return false;
        const auto it = curveIndex_.find(std::make_pair(slotIdx, paramId));
        return it != curveIndex_.end() &&
               curveSlots_[it->second].active.load(std::memory_order_acquire);
    }

    // ---- in-block curve scheduler -------------------------------------------
    // Message thread: an OSC set is a WAYPOINT, not an immediate latch. Stamp
    // it at the block the audio thread has NOT started yet (audioBlock_ + 2)
    // and push it into the per-(slot,param) SPSC ring; the audio thread drains
    // the ring at the start of each block and renders the block as a
    // sample-accurate sub-block staircase. The model value is updated
    // (no plugin latch) so /chain queries reflect the request; the plugin's
    // live value is owned solely by the audio staircase.
    //
    // curveSlots_ is a fixed-size vector built once in initCurveSlots() (one
    // entry per enabled slot's param). After init its size/pointer are stable,
    // so the audio thread may iterate it while the message thread pushes
    // waypoints. Each entry is single-owner: ring = SPSC (msg push / audio
    // drain), window+carry = audio thread (seeded by the msg thread BEFORE the
    // atomic active flag is published), active = atomic (msg sets / audio
    // reads). No locks anywhere.
    void initCurveSlots() {
        curveSlots_.clear();
        curveIndex_.clear();
        for (int s = 0; s < (int)cfg_.slots.size(); ++s) {
            if (!cfg_.slots[s].enabled) continue;
            const ParamSet* ps = chain.slot(s);
            if (ps == nullptr) continue;
            for (const Param& p : ps->params) {
                curveIndex_[std::make_pair(s, p.id)] = curveSlots_.size();
                // emplace (in-place default construction): CurveEntry is
                // non-movable (atomics), so push_back of a temporary won't
                // compile; deque emplace constructs the node in place.
                curveSlots_.emplace_back();
                curveSlots_.back().slotIdx = s;
                curveSlots_.back().paramId = p.id;
            }
        }
    }

    void scheduleCurveSet(int slotIdx, uint32_t paramId, float v01) {
        if (slotIdx < 0 || slotIdx >= (int)cfg_.slots.size()) return;
        const auto it = curveIndex_.find(std::make_pair(slotIdx, paramId));
        if (it == curveIndex_.end()) return;
        CurveEntry& e = curveSlots_[it->second];
        const float v = Param::clamp01(v01);
        const uint32_t curBlock = audioBlock_.load(std::memory_order_acquire);
        const ParamSet* ps = chain.slot(slotIdx);
        const Param* pp = (ps != nullptr) ? ps->find(paramId) : nullptr;
        // Anchor EVERY set at the current block, pinned to the value the audio
        // staircase is holding. The model value tracks the last requested value
        // (setParamModelById at the end of every set), which for a running curve
        // is exactly the value the staircase is holding, so `old` is the held
        // value for BOTH the first set after (de)activation (the plugin's current
        // value) and a follow-up set (the previous set's value). Pushed through
        // the ring -- the message thread never writes the audio-owned
        // window/carry, so there is no cross-thread write.
        //
        // Why on EVERY set, not just the first: without a per-set anchor, a
        // follow-up set's single far waypoint re-slants the chord from the
        // PREVIOUS waypoint backwards (retroactive bend). evalCurve then returns
        // the bent chord value instead of the held value -> a single-sample
        // click (measured: 0.0 -> 0.96875 = (71-9)/(73-9) at block 71). With the
        // per-set anchor, each ramp starts clean from the held value at "now"
        // and the value holds at "old" until the new ramp begins (no click).
        const float old = (pp != nullptr) ? pp->value : v;
        e.ring.push(curBlock, old);
        if (!e.active.load(std::memory_order_relaxed))
            e.active.store(true, std::memory_order_release);
        e.ring.push(curBlock + kLookahead, v);  // newest wins per block at drain
        if (dbgWin_ && dbgWinFile_)
            std::fprintf(dbgWinFile_, "SET slot=%d cur=%u anchor(%.1f,%.4f) "
                       "target(%.1f,%.4f)\n", slotIdx, curBlock,
                       (double)curBlock, old, (double)(curBlock + kLookahead),
                       v);
        chain.setParamModelById(slotIdx, paramId, v);
    }

    void clearCurvesForSlot(int slotIdx) {
        // Called with audio quiesced (handleReset / hot-apply), so deactivating
        // entries here cannot race a concurrent drain.
        for (auto& e : curveSlots_)
            if (e.slotIdx == slotIdx)
                e.active.store(false, std::memory_order_relaxed);
    }

    void clearAllCurves() {
        for (auto& e : curveSlots_)
            e.active.store(false, std::memory_order_relaxed);
    }

    // Audio thread (start of block `block`): drain every active curve's SPSC
    // ring into its ordered waypoint window (arrival order is non-decreasing in
    // block stamp, so append and compact same-block duplicates newest-wins),
    // then keep only the last waypoint at/BEFORE the current block (the
    // continuity anchor) plus all future waypoints -- everything older is dead
    // weight for evalCurve, which uses the nearest-left waypoint only. This
    // bounds the window to ~one ring of pending waypoints regardless of how
    // long a curve has been running. Rebuilds the per-slot working list.
    void prepareCurvesForBlock(uint32_t block) {
        slotsCurve_.clear();
        for (size_t i = 0; i < curveSlots_.size(); ++i) {
            CurveEntry& e = curveSlots_[i];
            if (!e.active.load(std::memory_order_acquire)) continue;
            heddle_InBlockCurve::Wp buf[512];
            const size_t n = e.ring.takeAll(buf, 512);
            if (n > 0) {
                auto& w = e.window;
                w.reserve(w.size() + n);
                for (size_t j = 0; j < n; ++j) {
                    if (!w.empty() && w.back().block == buf[j].block)
                        w.back().value = buf[j].value;   // newest wins
                    else
                        w.push_back(buf[j]);
                }
                // Drop everything older than the last completed waypoint.
                size_t keepFrom = w.size();
                for (size_t j = w.size(); j-- > 0;) {
                    if (w[j].block <= block) { keepFrom = j; break; }
                }
                if (keepFrom > 0) w.erase(w.begin(), w.begin() + keepFrom);
            }
            if (dbgWin_ && dbgWinFile_) {
                const float modelVal = (chain.slot(e.slotIdx) != nullptr)
                    ? (chain.slot(e.slotIdx)->find(e.paramId)
                           ? chain.slot(e.slotIdx)->find(e.paramId)->value : -1.f)
                    : -2.f;
                std::fprintf(dbgWinFile_, "B %u drain=%zu win=[", block, n);
                for (size_t j = 0; j < e.window.size(); ++j)
                    std::fprintf(dbgWinFile_, "%s(%.1f,%.4f)",
                        j ? " " : "", (double)e.window[j].block, e.window[j].value);
                std::fprintf(dbgWinFile_, "] carry=%.6f model=%.6f\n", e.carry, modelVal);
            }
            slotsCurve_.push_back(
                {e.slotIdx, {{e.paramId, i}}});
        }
    }

    // Audio thread: push every active param of `slotIdx` to the plugin at the
    // chord value for sub-block sample `p` (of `S`) within block `block`.
    void pushCurveAtSlot(int slotIdx, uint32_t block, uint32_t S, uint32_t p) {
        for (const auto& sc : slotsCurve_) {
            if (sc.first != slotIdx) continue;
            for (const auto& c : sc.second) {
                CurveEntry& e = curveSlots_[c.idx];
                const float v = heddle_InBlockCurve::chordAt(
                    e.window.data(), e.window.size(), block, S, p, e.carry);
                e.carry = v;                 // the plugin now latches v
                backend_.setParam(cfg_.slots[slotIdx].id, c.paramId, v);
            }
        }
    }

    // ---- OSC (message thread; model state is message-thread-only) ------------
    // No locks: the chain model, cfg_, curve tables, and the user-lock map
    // are written only on this thread. The one structural mutation (hot
    // apply, which swaps plugin instances) first quiesces the audio side.
    void reply(const std::string& addr, const std::string& text,
               const OscUdp::SourceAddr& src) {
        osc_.sendTo(OscMsg::make(addr, text), src);
    }

    void handleOsc(const OscMsg& msg, const OscUdp::SourceAddr& src) {
        const osc_addr::ParamAction a = osc_addr::classify(msg);
        switch (a.op) {
        case osc_addr::ParamAction::Op::QueryChain:
            handleQueryChain(src);
            return;
        case osc_addr::ParamAction::Op::QueryAll:
            handleQuerySlot(a.slotToken, src);
            return;
        case osc_addr::ParamAction::Op::SetByName:
        case osc_addr::ParamAction::Op::SetById:
            handleSet(a, src);
            return;
        case osc_addr::ParamAction::Op::ResetSlot:
            handleReset(a.slotToken, src);
            return;
        case osc_addr::ParamAction::Op::ResetAll:
            handleResetAll(src);
            return;
        case osc_addr::ParamAction::Op::ApplyPath:
            handleApply(a.blob, src, true);
            return;
        case osc_addr::ParamAction::Op::ApplyBlob:
            handleApply(a.blob, src, false);
            return;
        default:
            break;
        }
        // Host extensions (not part of the core classify() surface).
        if (msg.addr == "/chain/status") {
            reply("/reply/status", statusLine(), src);
        } else if (msg.addr == "/chain/quit") {
            reply("/reply/status", "quitting", src);
            quit_.store(true);
        }
    }

    void handleQueryChain(const OscUdp::SourceAddr& src) {
        std::string out;
        for (int i = 0; i < chain.numSlots(); ++i) {
            if (i > 0) out += "\n";
            std::string path;
            if (const ParamSet* ps = chain.slot(i))
                path = ps->loadedPath;
            if (path.empty())
                path = cfg_.slots[i].pluginPath;
            out += std::to_string(i) + "\t" + chain.slotName(i) + "\t" + path + "\t" +
                   (cfg_.slots[i].enabled ? "true" : "false");
        }
        reply("/reply/chain", out, src);
    }

    void handleQuerySlot(const std::string& token, const OscUdp::SourceAddr& src) {
        const int idx = chain.resolveSlot(token);
        const ParamSet* ps = idx >= 0 ? chain.slot(idx) : nullptr;
        if (ps == nullptr) {
            reply("/reply/" + token, "error: no such slot: " + token, src);
            return;
        }
        if (ps->params.empty()) {
            reply("/reply/" + ps->pluginId, "(no params)", src);
            return;
        }
        // One string message per param, at /reply/<slotId>/<paramTitle>.
        for (const OscMsg& m : osc_addr::paramSetToOsc(*ps))
            osc_.sendTo(m, src);
    }

    void handleSet(const osc_addr::ParamAction& a, const OscUdp::SourceAddr& src) {
        // In in-block mode the set becomes a WAYPOINT (the audio staircase owns
        // the plugin value); in legacy mode it latches immediately via
        // chain.setParam -> backend_.setParam (JUCE atomic cached value).
        const int idx = chain.resolveSlot(a.slotToken);
        const ParamSet* ps = idx >= 0 ? chain.slot(idx) : nullptr;
        if (ps == nullptr) {
            reply("/reply/" + a.slotToken, "error: param not found", src);
            return;
        }
        const Param* p = (a.op == osc_addr::ParamAction::Op::SetByName)
                             ? ps->findByName(a.param)
                             : ps->find(a.paramId);
        if (p == nullptr) {
            reply("/reply/" + a.slotToken, "error: param not found", src);
            return;
        }
        const std::string name = p->title;
        const float v = Param::clamp01(a.value);
        if (inblock_) {
            scheduleCurveSet(idx, p->id, v);
        } else {
            chain.setParamById(a.slotToken, p->id, v);
        }
        // Manual control outranks curve automation for this param.
        userLocked_[std::make_pair(idx, name)] = true;
        // Float ack at /reply/<slotId>/<paramTitle> (the documented contract).
        osc_.sendTo(OscMsg::make("/reply/" + ps->pluginId + "/" + name, v), src);
    }

    void handleReset(const std::string& token, const OscUdp::SourceAddr& src) {
        const int idx = chain.resolveSlot(token);
        if (idx < 0) {
            reply("/status", "error: no such slot", src);
            return;
        }
        const SlotConfig& sc = cfg_.slots[idx];
        // Model back to plugin defaults, then push the values into the live
        // plugin instance (a model-only reset would be inaudible).
        chain.resetSlot(idx);
        if (sc.enabled) {
            std::vector<std::pair<std::string, float>> vals;
            if (const ParamSet* ps = chain.slot(idx))
                for (const Param& p : ps->params)
                    vals.emplace_back(p.title, p.defaultValue);
            if (!vals.empty())
                backend_.setParams(sc.id, vals);
        }
        // Drop the user-lock / automation state for this slot.
        for (auto it = userLocked_.begin(); it != userLocked_.end();)
            it = (it->first.first == idx) ? userLocked_.erase(it) : ++it;
        for (auto it = prevCurve_.begin(); it != prevCurve_.end();)
            it = (it->first.first == idx) ? prevCurve_.erase(it) : ++it;
        // Drop any active in-block curve for this slot (atomic flag only; the
        // audio thread sees active=false on its next prepare and skips it).
        clearCurvesForSlot(idx);
        reply("/status", "ok slot reset: " + sc.id, src);
    }

    void handleResetAll(const OscUdp::SourceAddr& src) {
        chain.resetToDefaults();
        for (size_t i = 0; i < cfg_.slots.size(); ++i) {
            const SlotConfig& sc = cfg_.slots[i];
            if (!sc.enabled) continue;
            std::vector<std::pair<std::string, float>> vals;
            if (const ParamSet* ps = chain.slot(static_cast<int>(i)))
                for (const Param& p : ps->params)
                    vals.emplace_back(p.title, p.defaultValue);
            if (!vals.empty())
                backend_.setParams(sc.id, vals);
        }
        clearOverrides();
        clearAllCurves();
        reply("/status", "ok chain reset", src);
    }

    void handleApply(const std::string& payload, const OscUdp::SourceAddr& src, bool isPath) {
        std::string err;
        ChainConfig next;
        if (isPath) {
            if (!heddle::config::parseFile(payload, next, &err)) {
                reply("/status", "error: apply: " + err, src);
                return;
            }
        } else {
            if (!heddle::config::parse(payload, next, &err)) {
                reply("/status", "error: apply: " + err, src);
                return;
            }
        }
        // Structural swap: the hot-apply may destroy/recreate plugin
        // instances and replace cfg_, so wait for the in-flight audio block
        // (bounded by one block) before mutating. No audio frame is ever
        // skipped or silenced by this — the swap simply waits its turn.
        quiesceAudio();
        if (!chain.apply(next, &err)) {
            reply("/status", "error: apply: " + err, src);
            return;
        }
        cfg_ = next;
        buildAutomationTables();
        clearOverrides();
        // cfg_ just swapped (quiesced, audio not running): drop stale curves and
        // rebuild the curve entry table against the new config's param surface.
        clearAllCurves();
        if (inblock_) initCurveSlots();
        reply("/status", "ok chain applied (" + std::to_string(next.slots.size()) + " slots)",
              src);
    }

    std::string statusLine() const {
        return std::string("heddle-host v") + HEDDLE_VERSION_STRING + " slots=" +
               std::to_string(chain.numSlots()) + " audio=" + args_.audioMode +
               " frame=" + std::to_string(chainFrame_.load(std::memory_order_relaxed)) +
               " rate=" + std::to_string((int)specRate_) +
               " block=" + std::to_string(specBlock_);
    }

    // ---- model refresh (message thread): poll plugin param values ------------
    // The backend has no change callback in this build; plugin-internal
    // changes (in-plugin automation, an editor touched in a DAW) are picked
    // up here by polling the live values and mirrored into the model via
    // Chain::onPluginParamChanged (which also fires the /change push).
    // backend_.paramValue reads are JUCE thread-safe (atomic cache).
    void refreshModel() {
        for (int idx = 0; idx < chain.numSlots(); ++idx) {
            const ParamSet* ps = chain.slot(idx);
            if (ps == nullptr || ps->params.empty())
                continue;
            const SlotConfig& sc = cfg_.slots[idx];
            if (!sc.enabled)
                continue;
            for (const Param& modelP : ps->params) {
                // In-block mode: a param under an active curve is owned by the
                // audio staircase (which latches the live value sample-accurately,
                // lagging the scheduled target by up to kLookahead blocks). The
                // model value is the SCHEDULED (last-requested) value, not the
                // live value, so polling the plugin and mirroring it here would
                // clobber the scheduled value. The per-set anchor then reads a
                // stale live value and the next ramp clicks. Skip it -- the same
                // guard curveAutomationTick uses.
                if (isCurveActive(idx, modelP.id))
                    continue;
                const float v = backend_.paramValue(sc.id, modelP.id);
                if (v < 0.f)
                    continue;
                if (std::fabs(v - modelP.value) > 1e-5f)
                    chain.onPluginParamChanged(sc.id, modelP.id, v);
            }
        }
    }

    // ---- GUI (--gui; defined after the window classes at the bottom) ---------
    void guiStart();
    void guiPull();
    void guiContentChanged();

    // ---- teardown (idempotent) -------------------------------------------------
    void stopAll();  // out-of-line: guiWindow_ needs the complete
                     // HeddleHostWindow type (defined at the bottom).

    // ---- members ---------------------------------------------------------------
    Args args_;
    ChainConfig cfg_;
    heddle::JuceBackend backend_;
    Chain chain{backend_};
    juce::AudioDeviceManager adm_;
    OscUdp osc_;
    std::string oscErr_;
    std::string bindHost_ = "127.0.0.1";
    OscUdp::SourceAddr lastSrc_{};

    std::unique_ptr<NullClockThread> clock_;

    // Lock-free param path: no cross-thread mutex. Structural swaps
    // (hot-apply) instead use quiesceAudio(): the message thread spins
    // until the in-flight audio block (if any) finishes, then mutates
    // cfg_ / the backend. blockInFlight_ is set/cleared around the
    // callback body; the wait is bounded by one block (~1-21 ms).
    std::atomic<bool> blockInFlight_{false};

    juce::AudioBuffer<float> workIn_, workA_, workB_, workOut_;
    double specRate_ = 0.0;
    int specBlock_ = 0;
    double devRate_ = 0.0;
    int devBlock_ = 0;
    int automationPeriodMs_ = 5;   // dynamic: blockSize / (2*sampleRate), >= 5
    int64_t lastAutoMs_ = 0;       // 0 = re-arm the automation gate
    // Atomic: audio thread increments per processed block; the message
    // thread reads it (status line, curve evaluation).
    std::atomic<int64_t> chainFrame_{0};
    // Monotonic audio-block counter (audio thread increments once per
    // processed block; message thread reads it to stamp OSC sets 2 blocks
    // ahead). Distinct from chainFrame_ (which counts samples) because the
    // in-block curve scheduler reasons in block units.
    std::atomic<uint32_t> audioBlock_{0};

    // ---- in-block OSC curve scheduler (VST3 IParamValueQueue model) --------
    // A /chain/<slot>/<param> OSC set does NOT latch the plugin immediately;
    // it is stamped at block = audioBlock_ + kLookahead and pushed into a
    // per-(slot,param) SPSC ring. The audio thread drains the ring at the
    // start of each block, rebuilds the ordered waypoint window, and renders
    // the block as sub-block segments: at each segment boundary it pushes the
    // chord value for that sample to the plugin (setParam), then processes the
    // segment. This is what lets a single block carry a smooth in-block ramp
    // even though a VST3 plugin latches one value per processBlock call.
    // kLookahead = 2 keeps the current block (b) and next (b+1) committed
    // while every block >= b+2 stays bendable by a late-arriving set.
    static constexpr uint32_t kLookahead = 2;
    struct CurveEntry {
        int slotIdx = -1;                // immutable after init (audio reads)
        uint32_t paramId = 0;            // immutable after init (audio reads)
        heddle_InBlockCurve ring;        // SPSC waypoint ring (msg push / audio drain)
        std::vector<heddle_InBlockCurve::Wp> window;  // audio-thread ordered window
        float carry = 0.f;               // audio-thread carried value (hold)
        std::atomic<bool> active{false}; // under curve control (msg sets / audio reads)
    };
    // Fixed-size, built once in initCurveSlots() after chain.load (message
    // thread, audio not started). A deque (not a vector): CurveEntry contains
    // atomics and is non-movable, and deque element addresses are stable across
    // growth, so the audio thread's iteration stays valid. Each entry's fields
    // are single-owner (ring = SPSC; window/carry = audio thread; active =
    // atomic), so the audio thread may iterate while the message thread pushes
    // waypoints with no lock and no race.
    std::deque<CurveEntry> curveSlots_;
    // (slotIdx, paramId) -> index into curveSlots_. Message-thread-ONLY
    // (built at init, looked up in scheduleCurveSet) -- the audio thread never
    // reads it, so it needs no synchronisation.
    std::map<std::pair<int, uint32_t>, size_t> curveIndex_;
    // Audio-thread working set for the current block, rebuilt each block from
    // curveSlots_: slotIdx -> {paramId, index into curveSlots_} for every
    // ACTIVE curve. Rebuilt at the start of every block (prepareCurvesForBlock)
    // before any pushCurveAtSlot reads it. Index-based (not pointer-based) so a
    // hot-apply that reallocates curveSlots_ cannot dangle a captured address.
    struct SlotCurve { uint32_t paramId; size_t idx; };
    std::vector<std::pair<int, std::vector<SlotCurve>>> slotsCurve_;  // audio-thread rebuilt
    // In-block curve resolution (from cfg_.curveResolution / --substep).
    //   0  = whole-queue path: ONE raw process() per block with a two-point
    //        IParamValueQueue per active param; the plugin interpolates per-
    //        sample (smooth). The DEFAULT -- no oversampling.
    //   N>0 = oversample: split the block into N-sample sub-slices, each
    //        latching one control point (finer staircase -> chord).
    int curveSubstep_ = 4;
    // true = in-block curve scheduling (OSC sets become waypoints); false =
    // legacy immediate latch (an OSC set pushes the plugin value at once).
    bool inblock_ = true;
    // Debug: when set (env HEDDLE_DBG_WIN), the audio thread logs the per-block
    // waypoint window + v0/v1 to stderr for in-block scheduling forensics.
    bool dbgWin_ = false;
    std::FILE* dbgWinFile_ = nullptr;  // unbuffered; HEDDLE_DBG_FILE path

    // --audio ring: the two shared-memory rings (host owns both; the HeddleBridge
    // VST3 or the round-trip test attaches to them). ringIn_ = DAW -> host,
    // ringOut_ = host -> DAW. Only the pump thread touches the rings, so the
    // per-block scratch below needs no locking.
    heddle::AudioRing ringIn_, ringOut_;
    std::unique_ptr<AudioRingPumpThread> ringPump_;
    // ringInInter_ = interleaved samples read from ringIn_; de-interleaved into
    // the planar ringInBuf_ (ringInPtrs_[c] = channel-c pointer) for the chain.
    // ringOutBuf_ = interleaved samples written to ringOut_.
    std::vector<float> ringInInter_, ringInBuf_, ringOutBuf_;
    std::vector<const float*> ringInPtrs_;
    std::string ringInName_, ringOutName_;

    // slot index -> curved param names (automation only touches these).
    std::map<int, std::vector<std::string>> curveTables_;
    // (slot index, param name) -> last pushed curve value / user-locked flag.
    std::map<std::pair<int, std::string>, float> prevCurve_;
    std::map<std::pair<int, std::string>, bool> userLocked_;

    std::unique_ptr<HeddleHostWindow> guiWindow_;

    std::atomic<bool> quit_{false};
    bool running_ = false;
    bool stopped_ = false;
};

// ----------------------------------------------------------------------------
// --gui: Row - one line in the param list (a slot header, or a param slider)
// ----------------------------------------------------------------------------
// A param row copies the param's display metadata (title/units/range/labels)
// at build time, so it never points into the chain model (which is replaced on
// load/apply). The slider's onValueChange routes through
// HeddleHost::hostGuiSet (the OSC set path, user-locked); the row's value is
// re-synced from the model by ChainGuiContent::tick() every 5 ms tick.
class Row : public juce::Component {
public:
    // Param row.
    Row(HeddleHost& host, int slotIndex, const Param& p)
        : juce::Component("heddle-row"),
          host_(&host),
          slotIndex_(slotIndex),
          title_(p.title),
          units_(p.units),
          minRaw_(p.minValue),
          maxRaw_(p.maxValue),
          stepCount_(p.stepCount) {
        listItems_ = p.listItems;

        label.setText(title_, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::plain)));
        label.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(label);

        const double interval = (p.stepCount > 1) ? (1.0 / (p.stepCount - 1)) : 0.0;
        slider.setRange(0.0, 1.0, interval);
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 90, 22);
        slider.setValue(p.value, juce::dontSendNotification);
        slider.textFromValueFunction = [this](double norm) -> juce::String {
            return displayText(norm);
        };
        slider.valueFromTextFunction = [this](const juce::String& text) -> double {
            return parseText(text);
        };
        slider.onValueChange = [this]() {
            if (host_ != nullptr)
                host_->hostGuiSet(slotIndex_, title_, slider.getValue());
        };
        addAndMakeVisible(slider);
    }

    // Slot header row.
    explicit Row(const juce::String& text)
        : juce::Component("heddle-header"),
          host_(nullptr),
          isHeader_(true) {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(13.5f, juce::Font::bold)));
        label.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(label);
    }

    void resized() override {
        if (isHeader_) {
            label.setBounds(4, 0, std::max(1, getWidth() - 8), getHeight());
        } else {
            const int labelW = 130;
            label.setBounds(4, 0, labelW, getHeight());
            slider.setBounds(labelW + 8, 0,
                             std::max(1, getWidth() - labelW - 8), getHeight());
        }
    }

    bool isParamRow() const { return !isHeader_; }
    int slotIndex() const { return slotIndex_; }
    const std::string& title() const { return title_; }

    // Pull the model value into the slider (no onValueChange feedback).
    void setValueNoNotify(float norm) {
        slider.setValue(static_cast<double>(norm), juce::dontSendNotification);
    }

private:
    juce::String displayText(double norm) const {
        if (!listItems_.empty()) {
            const int n = (stepCount_ > 1) ? (stepCount_ - 1) : 1;
            int idx = static_cast<int>(std::lround(norm * static_cast<double>(n)));
            idx = juce::jlimit(0, static_cast<int>(listItems_.size()) - 1, idx);
            return juce::String(listItems_[idx]);
        }
        const double span = maxRaw_ - minRaw_;
        const double raw = (span <= 0.0) ? minRaw_ : minRaw_ + norm * span;
        juce::String s = juce::String(raw, 3);
        if (!units_.empty())
            s = s + " " + juce::String(units_);
        return s;
    }

    double parseText(const juce::String& text) const {
        const double raw = text.getDoubleValue();
        const double span = maxRaw_ - minRaw_;
        if (span <= 0.0)
            return 0.0;
        return juce::jlimit(0.0, 1.0, (raw - minRaw_) / span);
    }

    juce::Label label;
    juce::Slider slider;
    HeddleHost* host_ = nullptr;
    int slotIndex_ = 0;
    bool isHeader_ = false;
    std::string title_;
    std::string units_;
    float minRaw_ = 0.f;
    float maxRaw_ = 1.f;
    int stepCount_ = 0;
    std::vector<std::string> listItems_;
};

// ----------------------------------------------------------------------------
// --gui: ChainGuiContent - the window content (a scrollable list of Rows)
// ----------------------------------------------------------------------------
// Everything here runs on the JUCE message thread. Model reads happen under
// chainLock_ with a try-lock (skip the tick when the audio thread is mid-block).
// Rows copy their display metadata, so a rebuild is only triggered by a
// structural change (slot or param count), not by value changes.
class ChainGuiContent : public juce::Component {
public:
    explicit ChainGuiContent(HeddleHost& host)
        : juce::Component("heddle-gui"), host_(&host) {
        setOpaque(false);
        page_ = std::make_unique<juce::Component>("heddle-page");
        viewport_ = std::make_unique<juce::Viewport>("heddle-viewport");
        viewport_->setViewedComponent(page_.get(), false);
        viewport_->setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_.get());
        // Message thread; the chain model is message-thread-only, so the
        // initial snapshot needs no lock. (The window is opened before the
        // audio thread starts, so nothing else touches the model yet.)
        rebuildLocked();
    }

    // Called from HeddleHost::guiPull() (message thread).
    void tick() {
        rebuildIfStructureChanged();
        syncValues();
    }

    void resized() override {
        viewport_->setBounds(getLocalBounds());
        layoutPage(std::max(200, viewport_->getWidth()));
    }

    // Set an initial page size before the window is first laid out.
    void initialLayout() { layoutPage(420); }

private:
    void layoutPage(int width) {
        const int n = static_cast<int>(rows_.size());
        page_->setSize(width, std::max(1, n) * ROW_H);
        for (int i = 0; i < n; ++i)
            rows_[i]->setBounds(0, i * ROW_H, width, ROW_H);
    }

    // Rebuild the row list from the chain model. Message thread only.
    void rebuildLocked() {
        rows_.clear();  // deletes the rows; each auto-removes itself from page_
        const int nslots = host_->chain.numSlots();
        int nparams = 0;
        for (int i = 0; i < nslots; ++i) {
            const ParamSet* ps = host_->chain.slot(i);
            const std::string slotId =
                (ps != nullptr) ? ps->pluginId : host_->chain.slotName(i);
            const juce::String header =
                juce::String("Slot ") + juce::String(i) + juce::String(": ")
                + juce::String(slotId);
            auto row = std::make_unique<Row>(header);
            page_->addChildComponent(row.get());
            rows_.push_back(std::move(row));
            if (ps == nullptr)
                continue;
            for (const Param& p : ps->params) {
                auto prow = std::make_unique<Row>(*host_, i, p);
                page_->addChildComponent(prow.get());
                rows_.push_back(std::move(prow));
                ++nparams;
            }
        }
        sigSlots_ = nslots;
        sigParams_ = nparams;
    }

    void rebuildIfStructureChanged() {
        const int nslots = host_->chain.numSlots();
        int nparams = 0;
        for (int i = 0; i < nslots; ++i)
            if (const ParamSet* ps = host_->chain.slot(i))
                nparams += static_cast<int>(ps->params.size());
        if (nslots != sigSlots_ || nparams != sigParams_) {
            rebuildLocked();
            layoutPage(std::max(200, viewport_->getWidth()));
        }
    }

    void syncValues() {
        for (auto& row : rows_) {
            Row* r = row.get();
            if (!r->isParamRow())
                continue;
            const ParamSet* ps = host_->chain.slot(r->slotIndex());
            if (ps == nullptr)
                continue;
            const Param* p = ps->findByName(r->title());
            if (p == nullptr)
                continue;
            r->setValueNoNotify(p->value);
        }
    }

    HeddleHost* host_ = nullptr;
    std::unique_ptr<juce::Component> page_;
    std::unique_ptr<juce::Viewport> viewport_;
    std::vector<std::unique_ptr<Row>> rows_;
    int sigSlots_ = 0;
    int sigParams_ = 0;
    static constexpr int ROW_H = 28;
};

// ----------------------------------------------------------------------------
// --gui: HeddleHostWindow - the top-level resizable window
// ----------------------------------------------------------------------------
// Owns the ChainGuiContent (registered as the window's NON-owned content
// component). The window's tick() is driven by HeddleHost::guiPull() from the
// message thread. Closing the window requests a host quit (same as /chain/quit).
class HeddleHostWindow : public juce::ResizableWindow {
public:
    explicit HeddleHostWindow(HeddleHost& host)
        : juce::ResizableWindow("heddle-host", true), host_(&host) {
        setUsingNativeTitleBar(true);
        setResizable(true, false);
        setResizeLimits(320, 200, 1200, 1200);
        content_ = std::make_unique<ChainGuiContent>(host);
        setContentNonOwned(content_.get(), true);
        content_->initialLayout();
        centreWithSize(440, 520);
        setVisible(true);
    }

    ~HeddleHostWindow() override {
        // content_ is registered as a NON-owned content component. Drop the
        // window's reference BEFORE the unique_ptr member (and the base dtor)
        // run, so ~ResizableWindow's own clearContentComponent() is a no-op and
        // the content is deleted exactly once by content_.
        clearContentComponent();
    }

    void userTriedToCloseWindow() override {
        if (host_ != nullptr)
            host_->requestQuit();
    }

    // Message thread.
    void tick() {
        if (content_ != nullptr)
            content_->tick();
    }

private:
    HeddleHost* host_ = nullptr;
    std::unique_ptr<ChainGuiContent> content_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeddleHostWindow)
};

// ----------------------------------------------------------------------------
// HeddleHost::stopAll - defined out of line because guiWindow_ is a unique_ptr
// to HeddleHostWindow, which is an incomplete type inside the class body.
// ----------------------------------------------------------------------------
void HeddleHost::stopAll() {
    if (stopped_)
        return;
    stopped_ = true;
    stopTimer();
    guiWindow_.reset();  // the window's listeners touch host state: first
    if (clock_ != nullptr) {
        clock_->stopClock();
        clock_->stopThread(3000);
        clock_.reset();
    }
    if (ringPump_ != nullptr) {
        ringPump_->stopPump();
        ringPump_->stopThread(3000);
        ringPump_.reset();
    }
    // Join the device audio thread so no callback can touch the backend
    // while it is destroyed.
    if (adm_.getCurrentAudioDevice() != nullptr)
        adm_.removeAudioCallback(this);
    // The pump and device callback no longer run: the rings can be closed.
    // (On Windows the named mappings can't be deleted while live; on
    // POSIX the host's close() unmaps but the mapping object persists for
    // attached bridges until they detach too.)
    ringIn_.close();
    ringOut_.close();
    osc_.close();
    running_ = false;
    logInfo("stopped");
}

// ----------------------------------------------------------------------------
// HeddleHost --gui entry-point definitions (window classes need the complete
// HeddleHost type, so they sit here, after the class).
// ----------------------------------------------------------------------------
void HeddleHost::guiStart() {
    guiWindow_ = std::make_unique<HeddleHostWindow>(*this);
    logInfo("gui window open");
}

void HeddleHost::guiPull() {
    if (guiWindow_ != nullptr)
        guiWindow_->tick();
}

bool HeddleHost::hostGuiSet(int slotIndex, const std::string& name, double norm) {
    if (slotIndex < 0 || slotIndex >= chain.numSlots())
        return false;
    const std::string token = cfg_.slots[slotIndex].id;
    const ParamSet* ps = chain.slot(slotIndex);
    if (ps == nullptr)
        return false;
    const Param* p = ps->findByName(name);
    if (p == nullptr)
        return false;
    const float v = Param::clamp01((float)norm);
    // In in-block mode the GUI set becomes a waypoint (the audio staircase owns
    // the plugin value); otherwise latch immediately (JUCE thread-safe push).
    if (inblock_) {
        scheduleCurveSet(slotIndex, p->id, v);
    } else {
        chain.setParam(token, name, v);
    }
    // Manual control (GUI or OSC) outranks curve automation for this param.
    userLocked_[std::make_pair(slotIndex, name)] = true;
    return true;
}

void HeddleHost::requestQuit() {
    quit_.store(true);
}

int main(int argc, char** argv) {
    HeddleHost host;
    return host.run(argc, argv);
}
