// weft-host — the standalone live JUCE VST3 chain host.
//
//   weft-host <chain.json> [--audio null|system] [--device NAME]
//             [--osc-port N] [--duration SECONDS] [--version]
//
// Loads the chain described by the config JSON, instantiates the plugins
// via JuceBackend, and runs them live:
//
//   * --audio system (default): a real output device via AudioDeviceManager;
//     the JUCE audio callback drives the chain at the device's rate/block.
//   * --audio null: no audio device is touched; a software clock thread
//     drives the chain at the config's sample rate / block size. Used for
//     headless (CI) testing and OSC-only use.
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
// ONE juce::CriticalSection (chainLock_) guards the chain model AND the
// plugin instances (JuceBackend). The audio side (device callback, or the
// null-clock thread) takes it NON-blocking; on contention it outputs
// silence for that block. The message thread (OSC handlers, model refresh)
// takes it blocking. So there is exactly one writer at a time, and the
// audio never stalls on a message-thread reset.
//
// An OSC set marks the param "user-locked": curve automation stops touching
// it until /chain/<slot>/reset or a hot-apply.
//
// Headless pattern: plain main() + juce::initialiseJuce_GUI() — the same
// shape weft_smoke / weft-render already build with in CI (no
// JUCEApplication, no window). Timers are serviced by the
// MessageManager dispatch loop in run().

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
#include <juce_core/juce_core.h>

#include "JuceBackend.hpp"
#include "weft/chain.hpp"
#include "weft/config.hpp"
#include "weft/osc.hpp"
#include "weft/osc_io.hpp"

// Stamped by CMake (configure_file -> weft_version.hpp on this target's
// include path), same as weft_smoke / weft-render.
#include "weft_version.hpp"
#ifndef WEFT_VERSION_STRING
#define WEFT_VERSION_STRING "unknown"
#endif

using weft::Chain;
using weft::ChainConfig;
using weft::OscMsg;
using weft::OscUdp;
using weft::Param;
using weft::ParamSet;
using weft::ParamSpec;
using weft::SlotConfig;
namespace osc_addr = weft::osc_addr;

namespace {

void logInfo(const std::string& msg) {
    std::fprintf(stderr, "[weft-host] %s\n", msg.c_str());
    std::fflush(stderr);
}

void logErr(const std::string& msg) {
    std::fprintf(stderr, "[weft-host] ERROR: %s\n", msg.c_str());
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
    NullClockThread() : juce::Thread("weft-null-clock") {}

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

}  // namespace

// ----------------------------------------------------------------------------
// WeftHost
// ----------------------------------------------------------------------------
class WeftHost : public juce::AudioIODeviceCallback,
                 public juce::Timer {
public:
    WeftHost() = default;
    ~WeftHost() override { stopAll(); }

    int run(int argc, char** argv) {
        if (!parseArgs(argc, argv))
            return 1;
        if (args_.printVersion)
            return 0;

        // The main thread becomes the JUCE message thread; all JuceBackend
        // (plugin) calls below happen on it.
        juce::initialiseJuce_GUI();

        std::string err;
        if (!weft::config::parseFile(args_.configPath, cfg_, &err)) {
            logErr("config: " + err);
            return 1;
        }
        if (cfg_.slots.empty()) {
            logErr("config has no slots");
            return 1;
        }
        if (!chain.load(cfg_, &err)) {
            logErr("chain load failed: " + err);
            return 1;
        }
        buildAutomationTables();
        // Plugin-internal changes (detected by refreshModel()) are pushed to
        // the last OSC peer as /change/<slot>/<param>.
        chain.onChange([this](int slotIndex, const Param& p) {
            osc_.sendTo(OscMsg::make(osc_addr::makeChangeAddr(slotIndex, p.title), p.value),
                        lastSrc_);
        });

        if (args_.audioMode == "system") {
            if (!initAudioDevice(&err)) {
                logErr(err);
                return 1;
            }
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

        logInfo("ready, v" WEFT_VERSION_STRING ", " + std::to_string(chain.numSlots()) +
                " slot(s), OSC UDP " + bindHost_ + ":" + std::to_string(osc_.localPort()) +
                (args_.durationSec > 0.0 ? ", stopping in " + fmtF(args_.durationSec) + " s"
                                         : ""));

        startTimer(5);  // 5 ms OSC poll from the message thread

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
        const int frames =
            nullTick ? (cfg_.blockSize > 0 ? cfg_.blockSize : 512) : numSamples;
        const double rate =
            nullTick ? (cfg_.sampleRate > 0 ? static_cast<double>(cfg_.sampleRate) : 44100.0)
                     : (devRate_ > 0.0 ? devRate_ : static_cast<double>(cfg_.sampleRate));

        // Contention policy: the message thread may be mid-reset/apply; never
        // block the audio for it. Silence for this block and move on.
        juce::GenericScopedTryLock<juce::CriticalSection> tryLock(chainLock_);
        if (!tryLock.isLocked()) {
            if (outData != nullptr)
                for (int c = 0; c < numOuts; ++c)
                    if (outData[c] != nullptr)
                        std::fill(outData[c], outData[c] + frames, 0.f);
            return;
        }

        if (specRate_ != rate || specBlock_ != frames) {
            specRate_ = rate;
            specBlock_ = frames;
            backend_.setProcessSpec(rate, frames);
        }

        runBlockLocked(inData, numIns, frames);
        chainFrame_ += frames;

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
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        if (device != nullptr) {
            devRate_ = device->getCurrentSampleRate();
            devBlock_ = device->getCurrentBufferSizeSamples();
        }
        running_ = true;
        logInfo("audio device started (" + std::to_string((int)devRate_) + " Hz, " +
                std::to_string(devBlock_) + "-sample blocks)");
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
        refreshModel();
    }

private:
    // ---- args ---------------------------------------------------------------
    struct Args {
        std::string configPath;
        std::string audioMode = "system";
        std::string deviceName;
        int oscPort = 0;            // 0 = not given on the CLI
        bool printVersion = false;
        double durationSec = 0.0;
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
            } else if (a == "--osc-port") {
                const char* v = need("--osc-port");
                if (v == nullptr) return false;
                args_.oscPort = std::atoi(v);
            } else if (a == "--duration") {
                const char* v = need("--duration");
                if (v == nullptr) return false;
                args_.durationSec = std::atof(v);
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
            std::printf("weft-host %s\n", WEFT_VERSION_STRING);
            return true;
        }
        if (args_.configPath.empty()) {
            logErr("missing config path");
            usage(argv[0]);
            return false;
        }
        if (args_.audioMode != "system" && args_.audioMode != "null") {
            logErr("unknown --audio '" + args_.audioMode + "' (want 'system' or 'null')");
            return false;
        }
        return true;
    }

    static void usage(const char* prog) {
        std::fprintf(stderr,
            "usage: %s <chain.json> [--audio null|system] [--device NAME]\n"
            "       [--osc-port N] [--duration SECONDS] [--version]\n"
            "  Live VST3 chain host (JUCE). OSC control over UDP; the config's\n"
            "  osc.port/bind_host are the defaults, --osc-port overrides the port.\n",
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

    // ---- per-block processing (chainLock_ held) ------------------------------
    void runBlockLocked(const float* const* inData, int numIns, int frames) {
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

        // Curve automation: only curved params, skip user-locked ones.
        for (const auto& kv : curveTables_) {
            const int slotIdx = kv.first;
            if (slotIdx < 0 || slotIdx >= (int)cfg_.slots.size()) continue;
            const SlotConfig& sc = cfg_.slots[slotIdx];
            if (!sc.enabled) continue;
            std::vector<std::pair<std::string, float>> changed;
            for (const auto& name : kv.second) {
                const ParamSpec* spec = sc.paramSpec(name);
                if (spec == nullptr) continue;
                const float v = Param::clamp01(spec->at(chainFrame_));
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

        // Ping-pong the enabled slots (same idiom as render.cpp).
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
        if (cur != nullptr)
            workOut_.copyFrom(0, 0, *cur, 0, 0, frames);
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

    // ---- OSC (message thread; chainLock_ held by every handler) --------------
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
            juce::ScopedLock lock(chainLock_);
            reply("/reply/status", statusLine(), src);
        } else if (msg.addr == "/chain/quit") {
            reply("/reply/status", "quitting", src);
            quit_.store(true);
        }
    }

    void handleQueryChain(const OscUdp::SourceAddr& src) {
        juce::ScopedLock lock(chainLock_);
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
        juce::ScopedLock lock(chainLock_);
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
        juce::ScopedLock lock(chainLock_);
        const float v = (a.op == osc_addr::ParamAction::Op::SetByName)
                            ? chain.setParam(a.slotToken, a.param, a.value)
                            : chain.setParamById(a.slotToken, a.paramId, a.value);
        const int idx = chain.resolveSlot(a.slotToken);
        const ParamSet* ps = idx >= 0 ? chain.slot(idx) : nullptr;
        if (v < 0.f || ps == nullptr) {
            reply("/reply/" + a.slotToken, "error: param not found", src);
            return;
        }
        const Param* p = (a.op == osc_addr::ParamAction::Op::SetByName)
                             ? ps->findByName(a.param)
                             : ps->find(a.paramId);
        const std::string name = (p != nullptr) ? p->title : a.param;
        // Manual control outranks curve automation for this param.
        userLocked_[std::make_pair(idx, name)] = true;
        // Float ack at /reply/<slotId>/<paramTitle> (the documented contract).
        osc_.sendTo(OscMsg::make("/reply/" + ps->pluginId + "/" + name, v), src);
    }

    void handleReset(const std::string& token, const OscUdp::SourceAddr& src) {
        juce::ScopedLock lock(chainLock_);
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
        reply("/status", "ok slot reset: " + sc.id, src);
    }

    void handleResetAll(const OscUdp::SourceAddr& src) {
        juce::ScopedLock lock(chainLock_);
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
        reply("/status", "ok chain reset", src);
    }

    void handleApply(const std::string& payload, const OscUdp::SourceAddr& src, bool isPath) {
        std::string err;
        ChainConfig next;
        if (isPath) {
            if (!weft::config::parseFile(payload, next, &err)) {
                reply("/status", "error: apply: " + err, src);
                return;
            }
        } else {
            if (!weft::config::parse(payload, next, &err)) {
                reply("/status", "error: apply: " + err, src);
                return;
            }
        }
        juce::ScopedLock lock(chainLock_);
        if (!chain.apply(next, &err)) {
            reply("/status", "error: apply: " + err, src);
            return;
        }
        cfg_ = next;
        buildAutomationTables();
        clearOverrides();
        reply("/status", "ok chain applied (" + std::to_string(next.slots.size()) + " slots)",
              src);
    }

    std::string statusLine() const {
        return std::string("weft-host v") + WEFT_VERSION_STRING + " slots=" +
               std::to_string(chain.numSlots()) + " audio=" + args_.audioMode +
               " frame=" + std::to_string(chainFrame_) +
               " rate=" + std::to_string((int)specRate_) +
               " block=" + std::to_string(specBlock_);
    }

    // ---- model refresh (message thread): poll plugin param values ------------
    // The backend has no change callback in this build; plugin-internal
    // changes (in-plugin automation, an editor touched in a DAW) are picked
    // up here by polling the live values and mirrored into the model via
    // Chain::onPluginParamChanged (which also fires the /change push).
    void refreshModel() {
        juce::GenericScopedTryLock<juce::CriticalSection> tryLock(chainLock_);
        if (!tryLock.isLocked())
            return;  // audio side is mid-block; skip this tick
        for (int idx = 0; idx < chain.numSlots(); ++idx) {
            const ParamSet* ps = chain.slot(idx);
            if (ps == nullptr || ps->params.empty())
                continue;
            const SlotConfig& sc = cfg_.slots[idx];
            if (!sc.enabled)
                continue;
            for (const Param& modelP : ps->params) {
                const float v = backend_.paramValue(sc.id, modelP.id);
                if (v < 0.f)
                    continue;
                if (std::fabs(v - modelP.value) > 1e-5f)
                    chain.onPluginParamChanged(sc.id, modelP.id, v);
            }
        }
    }

    // ---- teardown (idempotent) -------------------------------------------------
    void stopAll() {
        if (stopped_)
            return;
        stopped_ = true;
        stopTimer();
        if (clock_ != nullptr) {
            clock_->stopClock();
            clock_->stopThread(3000);
            clock_.reset();
        }
        // Join the device audio thread so no callback can touch the backend
        // while it is destroyed.
        if (adm_.getCurrentAudioDevice() != nullptr)
            adm_.removeAudioCallback(this);
        osc_.close();
        running_ = false;
        logInfo("stopped");
    }

    // ---- members ---------------------------------------------------------------
    Args args_;
    ChainConfig cfg_;
    weft::JuceBackend backend_;
    Chain chain{backend_};
    juce::AudioDeviceManager adm_;
    OscUdp osc_;
    std::string oscErr_;
    std::string bindHost_ = "127.0.0.1";
    OscUdp::SourceAddr lastSrc_{};

    std::unique_ptr<NullClockThread> clock_;
    juce::CriticalSection chainLock_;

    juce::AudioBuffer<float> workIn_, workA_, workB_, workOut_;
    double specRate_ = 0.0;
    int specBlock_ = 0;
    double devRate_ = 0.0;
    int devBlock_ = 0;
    int64_t chainFrame_ = 0;

    // slot index -> curved param names (automation only touches these).
    std::map<int, std::vector<std::string>> curveTables_;
    // (slot index, param name) -> last pushed curve value / user-locked flag.
    std::map<std::pair<int, std::string>, float> prevCurve_;
    std::map<std::pair<int, std::string>, bool> userLocked_;

    std::atomic<bool> quit_{false};
    bool running_ = false;
    bool stopped_ = false;
};

int main(int argc, char** argv) {
    WeftHost host;
    return host.run(argc, argv);
}
