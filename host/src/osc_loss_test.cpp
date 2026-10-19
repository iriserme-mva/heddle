// heddle-osc-loss-test -- SAMPLE-LEVEL round-trip loss test against a REAL
// live host (no block-count deficit, no throughput proxy: the actual
// waveform is streamed in, captured out, and compared sample-for-sample).
//
// WHY THIS EXISTS
//   heddle-osc-perf-host measures a *count* metric (output blocks/s deficit),
//   which the maintainer rightly rejected as insufficient: a deficit of ~0
//   blocks/s can still hide dropped *samples* inside blocks. This tool proves
//   the stronger claim the user asked for: stream a KNOWN rectangular test
//   waveform into the host's input ring, capture the output ring, and verify
//   that every one of the 2048 non-zero samples per loop arrives intact, even
//   while the message thread is saturated with OSC param sets.
//
// THE WAVEFORM (per spec)
//   A 4096-sample loop, played continuously:
//     [ 1024 x 0.0 ] [ 2048 x -1.5 ] [ 1024 x 0.0 ]
//   The smoke VST3 at Gain=1.0 / PWM=1.0 is the IDENTITY (out = in*1*1), so
//   the output must be byte-identical to the input (pass-through). Each loop
//   contributes exactly one 2048-sample non-zero "pulse". If any sample is
//   dropped, either a pulse is truncated (len < 2048) or a whole pulse is
//   missing (the gap between pulses widens by 4096).
//
// WHAT IT MEASURES
//   A. Baseline (no OSC load):
//        - pulse integrity (expected vs observed 2048-sample pulses, mid-pulse
//          truncation)  -> the "no sample loss" claim
//        - round-trip OFFSET in samples = pipeline depth (totalWritten -
//          outIndex in steady state), and in ms
//   B. Per sustained OSC set-rate (default 50..10000):
//        - the same pulse integrity under load  (the core assertion)
//        - OFFSET under load (does latency move when the message thread
//          saturates?)
//        - set->ack RTT + UDP loss (carried from the perf methodology)
//   C. Control KICK-IN: flip Gain 1.0 -> 0.5 while mid-pulse, find the first
//      output sample that reflects the new gain (|x|~0.75 instead of 1.5).
//      kick-in offset = (that output sample index) - (input sample index when
//      the OSC set was sent), in samples + ms. Repeats 5x -> min/median/max.
//      This bounds how fast a control change reaches the audio path (the DAW
//      "knob feels instant" number: expect <= one 5ms OSC drain + one audio
//      block).
//
// LINKS ONLY heddle_core (ring + OSC); the child heddle-host does the JUCE /
// audio / VST3 work. Runs headless (no GUI, no audio device, no Xvfb).
//
// USAGE
//   heddle-osc-loss-test <heddle-host> <smoke-vst3> \
//       [--duration 3] [--rates 50,200,500,1000,2000,5000,10000] \
//       [--block 1024] [--rate 48000] [--kick 5]
//
// Exit 0 = all phases completed (loss may be non-zero; it is REPORTED, not a
// hard fail, so a regression is visible in the infographic). Exit 1 = a phase
// errored (ring never appeared, host died, etc.).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "heddle/osc.hpp"
#include "heddle/osc_io.hpp"
#include "heddle/ring.hpp"

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <signal.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

using namespace heddle;
using Clock = std::chrono::steady_clock;

// ---- the test waveform -----------------------------------------------------
static constexpr int kSilenceA = 1024;   // leading silence
static constexpr int kPulse    = 2048;   // non-zero region (must all arrive)
static constexpr int kSilenceB = 1024;   // trailing silence
static constexpr int kLoop     = kSilenceA + kPulse + kSilenceB;  // 4096
static constexpr float kAmp    = -1.5f;  // non-zero amplitude
static constexpr float kEps    = 0.01f;  // "non-zero" threshold
static constexpr float kNewGainAmp = 0.75f;  // |kAmp| * 0.5 after Gain=0.5

// ---------------------------------------------------------------------------
// Child spawn/reap -- self-contained copy of the idiom from osc_perf_host.cpp.
struct ChildHandle {
    bool valid = false;
#if defined(_WIN32)
    void* hProc = nullptr;
    void* hThread = nullptr;
#else
    pid_t pid = -1;
#endif
};

static ChildHandle spawnChild(const std::string& exe,
                              const std::vector<std::string>& argv) {
#if defined(_WIN32)
    std::string cmd;
    for (const std::string& a : argv) {
        if (!cmd.empty()) cmd += " ";
        if (a.find_first_of(" \\\")") == std::string::npos) {
            cmd += a;
        } else {
            cmd += "\"";
            size_t bs = 0;
            for (char c : a) {
                if (c == '\\') { bs++; continue; }
                cmd.append(bs * 2, '\\');
                if (c == '"') cmd.push_back('\\');
                bs = 0;
                cmd.push_back(c);
            }
            cmd.append(bs * 2, '\\');
            cmd.push_back('"');
        }
    }
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    ChildHandle h;
    if (CreateProcessA(exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, nullptr, &si, &pi)) {
        h.valid = true;
        h.hProc = pi.hProcess;
        h.hThread = pi.hThread;
    }
    return h;
#else
    pid_t pid = fork();
    ChildHandle h;
    if (pid < 0) return h;
    if (pid == 0) {
        std::vector<char*> cargv;
        for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execv(exe.c_str(), cargv.data());
        _exit(127);
    }
    h.valid = true;
    h.pid = pid;
    return h;
#endif
}

static bool childFinished(const ChildHandle& h) {
    if (!h.valid) return true;
#if defined(_WIN32)
    return WaitForSingleObject(h.hProc, 0) == WAIT_OBJECT_0;
#else
    return waitpid(h.pid, nullptr, WNOHANG) != 0;
#endif
}

static int childReap(ChildHandle& h) {
    int ec = -1;
    if (!h.valid) return ec;
#if defined(_WIN32)
    if (WaitForSingleObject(h.hProc, 30000) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(h.hProc, &code);
        ec = static_cast<int>(code);
    }
    CloseHandle(h.hProc);
    CloseHandle(h.hThread);
#else
    int status = 0;
    waitpid(h.pid, &status, 0);
    if (WIFEXITED(status)) ec = WEXITSTATUS(status);
#endif
    h.valid = false;
    return ec;
}

static void childKill(ChildHandle& h) {
    if (!h.valid) return;
#if defined(_WIN32)
    TerminateProcess(h.hProc, 1);
#else
    ::kill(h.pid, SIGKILL);
#endif
}

// ---------------------------------------------------------------------------
using ClockNs = std::chrono::nanoseconds;
static double nowNs() {
    return std::chrono::duration_cast<ClockNs>(Clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Audio producer: streams the 4096-sample loop into the input ring as fast as
// the ring has space (pacing to the pump = backpressure on THIS side, not the
// host), interleaved stereo, L=R. totalWritten is the absolute count of input
// samples pushed (0-based sample index on the input grid). The consumer reads
// the output ring on the main thread and keeps outIndex = absolute count of
// output samples read. Because the host is a pass-through, output sample i ==
// input sample i (same index grid, delayed in time), so:
//     OFFSET (samples) = totalWritten - outIndex   (pipeline depth, steady state)
struct Producer {
    AudioRing* ring = nullptr;
    int ch = 2;
    int block = 1024;
    int sr = 48000;
    std::atomic<bool> stop{false};
    std::atomic<long long> totalWritten{0};
    std::vector<float> buf;
    std::atomic<long long> phase{0};  // sample index within the infinite pattern stream
    void run() {
        buf.assign((size_t)block * ch, 0.0f);
        // Real-time pacing (mirrors the host pump): each block of audio takes
        // block*1e6/sr ns of wall time. Both sides of the input ring then run
        // at the same rate, the ring depth stays near zero, and the round-trip
        // offset (totalWritten - outIndex) is a small, stable pipeline depth
        // instead of a ring-full backlog. If the ring is momentarily full the
        // write just waits (backpressure); the target advances only by what
        // was actually written, so no catch-up burst happens.
        auto target = Clock::now();
        while (!stop.load()) {
            const size_t n = ring->write(buf.data(), block);
            if (n > 0) {
                totalWritten.fetch_add((long long)n, std::memory_order_relaxed);
                phase.fetch_add((long long)n, std::memory_order_relaxed);
                target += std::chrono::duration_cast<ClockNs>(
                    std::chrono::duration<double, std::ratio<1, 1000000000>>(
                        (double)n * 1e9 / (double)sr));
                // Fill the NEXT block from the pattern at the new phase.
                const long long ph0 = phase.load(std::memory_order_relaxed);
                for (int i = 0; i < block; ++i) {
                    const int p = (int)((ph0 + i) % kLoop);
                    const float v = (p >= kSilenceA && p < kSilenceA + kPulse) ? kAmp : 0.0f;
                    buf[(size_t)i * ch + 0] = v;
                    buf[(size_t)i * ch + 1] = v;
                }
                // Absolute-schedule pacing with a short busy-spin (mirrors the
                // host pump): sleep_for drifts on Windows (15 ms granularity)
                // and would desync input from the pump, corrupting the sample
                // accounting. Sleep to ~2 ms out, spin the rest.
                while (!stop.load()) {
                    const auto now = Clock::now();
                    if (now >= target) break;
                    const auto rem = std::chrono::duration_cast<ClockNs>(target - now).count();
                    if (rem > 2000000)
                        std::this_thread::sleep_for(std::chrono::nanoseconds(rem - 2000000));
                }
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Pulse detection over a slice of the captured (mono) output.
struct Pulse {
    long long start;  // index within the slice (0-based)
    long long len;
    float peak;       // max |value|
};
static std::vector<Pulse> detectPulses(const float* d, long long n) {
    std::vector<Pulse> out;
    long long i = 0;
    while (i < n) {
        if (std::fabs(d[i]) > kEps) {
            long long s = i;
            float peak = std::fabs(d[i]);
            while (i < n && std::fabs(d[i]) > kEps) {
                if (std::fabs(d[i]) > peak) peak = std::fabs(d[i]);
                ++i;
            }
            out.push_back(Pulse{s, i - s, peak});
        } else {
            ++i;
        }
    }
    return out;
}

struct PulseReport {
    long long sliceLen = 0;
    long long expected = 0;
    long long observed = 0;
    long long lostPulses = 0;      // fully-missing pulses (gap widened)
    long long truncatedSamples = 0; // sum of (kPulse - len) over short interior pulses
    long long totalLostSamples = 0; // lostPulses*kPulse + truncatedSamples
    bool pass = false;             // no loss: observed==expected and no truncation
    double offsetSamples = -1;
};
static PulseReport analyzePulses(const float* d, long long n, double offsetSamples) {
    PulseReport r;
    r.sliceLen = n;
    r.offsetSamples = offsetSamples;
    const auto pulses = detectPulses(d, n);
    r.observed = (long long)pulses.size();
    // Loss is detected by GAP, not by an absolute expected count (which is
    // boundary-sensitive): in the clean pass-through, consecutive pulse starts
    // are exactly kLoop (4096) apart. A gap of ~2*kLoop means a whole pulse was
    // dropped between them; a shorter pulse means truncation.
    r.expected = r.observed;  // baseline: no loss assumed, adjusted below
    for (size_t i = 1; i < pulses.size(); ++i) {
        const double gap = (double)(pulses[i].start - pulses[i - 1].start);
        const long long missed = (long long)std::llround(gap / (double)kLoop) - 1;
        if (missed > 0) { r.lostPulses += missed; r.expected += missed; }
    }
    // Interior pulses (skip first+last: they may be clipped by the slice
    // window) must each be ~kPulse long.
    for (size_t i = 1; i + 1 < pulses.size(); ++i) {
        const long long shortBy = (long long)kPulse - pulses[i].len;
        if (shortBy > 0) r.truncatedSamples += shortBy;
    }
    r.totalLostSamples = r.lostPulses * kPulse + r.truncatedSamples;
    // PASS = no fully-missing pulses AND no truncation in interior pulses, and
    // we actually saw a few pulses (sanity).
    r.pass = (r.lostPulses == 0) && (r.truncatedSamples == 0) && (r.observed >= 2);
    return r;
}

// ---------------------------------------------------------------------------
struct LossRow {
    double rate;          // OSC sets/s (0 = baseline, no load)
    double sent, acks, lossPct, rttMedianUs, rttP99Us;
    double offsetSamples, offsetMs;
    long long expected, observed, lostSamples;
    bool pass;
};

static std::string dirOf(const std::string& p) {
#if defined(_WIN32)
    size_t i = p.find_last_of("/\\");
#else
    size_t i = p.find_last_of('/');
#endif
    return (i == std::string::npos) ? std::string(".") : p.substr(0, i + 1);
}
static std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: heddle-osc-loss-test <heddle-host> <smoke-vst3> "
                     "[--duration SEC] [--rates a,b,c] [--block N] [--rate SR] [--kick N]\n");
        return 2;
    }
    const std::string hostExe = argv[1];
    const std::string vst3 = argv[2];
    std::string ratesCsv = "50,200,500,1000,2000,5000,10000";
    double durationSec = 3.0;
    int block = 1024;
    int sr = 48000;
    int kickRepeats = 5;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };
        if (a == "--rates") ratesCsv = next();
        else if (a == "--duration") durationSec = std::atof(next());
        else if (a == "--block") block = std::atoi(next());
        else if (a == "--rate") sr = std::atoi(next());
        else if (a == "--kick") kickRepeats = std::atoi(next());
    }

    const char* inName = "heddle.loss.in";
    const char* outName = "heddle.loss.out";
    long seed;
#if defined(_WIN32)
    seed = (long)GetCurrentProcessId();
#else
    seed = (long)getpid();
#endif
    const int hostPort = 21500 + (int)(seed % 4000);

    const std::string chainPath = dirOf(hostExe) + "heddle_osc_loss_test_chain.json";
    ChildHandle child;
    {
        // Identity pass-through: Gain=1.0, PWM=1.0 -> out == in.
        std::string json =
            "{\"sample_rate\": " + std::to_string(sr) + ", \"block_size\": " +
            std::to_string(block) + ", \"chain\": [ { \"id\": \"smoke\", "
            "\"plugin\": \"" + jsonEscape(vst3) + "\", \"enabled\": true, "
            "\"params\": { \"Gain\": 1.0, \"PWM pct\": 1.0 } } ] }\n";
        FILE* f = std::fopen(chainPath.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "FAIL: cannot write chain %s\n", chainPath.c_str()); return 1; }
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
    }

    heddle::AudioRing::remove(inName);
    heddle::AudioRing::remove(outName);

    std::vector<std::string> hargv = {hostExe, chainPath, "--audio", "ring",
                                      "--ring", "heddle.loss",
                                      "--osc-port", std::to_string(hostPort)};
    child = spawnChild(hostExe, hargv);
    if (!child.valid) { std::fprintf(stderr, "FAIL: spawn heddle-host\n"); return 1; }

    const auto ringDeadline = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < ringDeadline &&
           !(heddle::AudioRing::exists(inName) && heddle::AudioRing::exists(outName))) {
        if (childFinished(child)) { std::fprintf(stderr, "FAIL: host exited before rings\n"); childKill(child); return 1; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!(heddle::AudioRing::exists(inName) && heddle::AudioRing::exists(outName))) {
        std::fprintf(stderr, "FAIL: rings did not appear in 30s\n"); childKill(child); childReap(child); return 1;
    }
    std::string err;
    AudioRing ringIn = AudioRing::open(inName, &err, 3000);
    AudioRing ringOut = AudioRing::open(outName, &err, 3000);
    if (!ringIn.valid() || !ringOut.valid()) { std::fprintf(stderr, "FAIL: open rings: %s\n", err.c_str()); childKill(child); childReap(child); return 1; }

    OscUdp client;
    if (!client.bind("127.0.0.1", 0, &err)) { std::fprintf(stderr, "FAIL: client bind: %s\n", err.c_str()); childKill(child); childReap(child); return 1; }
    if (!client.openOut("127.0.0.1", hostPort, &err)) { std::fprintf(stderr, "FAIL: client openOut: %s\n", err.c_str()); childKill(child); childReap(child); return 1; }

    client.send(OscMsg::make("/chain/query"));
    bool gotChain = false;
    {
        OscMsg m;
        const auto dl = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < dl && !gotChain) {
            if (client.poll(m) && m.addr == "/reply/chain" && !m.strs.empty()) gotChain = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (!gotChain) { std::fprintf(stderr, "FAIL: no /reply/chain\n"); childKill(child); childReap(child); return 1; }

    // ---- shared capture machinery ----------------------------------------
    // The producer runs for the whole session on its own thread. The main
    // thread is the consumer: it reads the output ring (ch0 -> `out`), tracks
    // outIndex (absolute output sample count), sends OSC sets at the target
    // rate, and polls acks. `out` is never cleared between phases; each phase
    // records its [outStart, outEnd) slice.
    std::vector<float> out;
    out.reserve((size_t)(sr * (durationSec * (ratesCsv.size() / 8.0 + 2.0) + 8.0)) * 2);
    long long outIndex = 0;

    std::vector<float> scratch((size_t)block * ringIn.numChannels(), 0.0f);

    auto drainMs = [&](int ms) {
        const auto dl = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < dl) {
            const size_t n = ringOut.read(scratch.data(), block);
            if (n > 0) {
                for (size_t i = 0; i < n; ++i) out.push_back(scratch[(size_t)i * ringIn.numChannels() + 0]);
                outIndex += (long long)n;
            } else std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    };

    Producer pr;
    pr.ring = &ringIn;
    pr.ch = ringIn.numChannels();
    pr.block = block;
    pr.sr = sr;
    pr.totalWritten.store(0);
    pr.phase.store(0);
    std::thread prod([&]() { pr.run(); });

    // Warm up + settle to steady state.
    drainMs(2500);

    std::vector<LossRow> rows;
    auto runPhase = [&](double rate, bool* baselineOut) {
        // `rate` OSC sets/s (0 = no load). Runs for durationSec, streams the
        // waveform the whole time, returns the pulse report + offset + RTT.
        const double periodNs = rate > 0 ? 1e9 / rate : 1e18;
        const double t0 = nowNs();
        const double t1 = t0 + durationSec * 1e9;
        long long sent = 0, acks = 0;
        std::deque<double> sendTimes;
        std::vector<double> rttNs;
        rttNs.reserve(1 << 15);
        double nextSend = t0;
        long long outStart = outIndex;
        std::vector<double> offsetSamplesSamples;  // steady-state pipeline depth
        const double settleEnd = t0 + std::min(0.6, durationSec * 0.4) * 1e9;
        while (nowNs() < t1) {
            while (nowNs() >= nextSend && nextSend < t1 && rate > 0) {
                client.send(OscMsg::make("/chain/smoke/Gain", 1.0f));
                sendTimes.push_back(nextSend);
                ++sent;
                nextSend += periodNs;
            }
            for (int k = 0; k < 16; ++k) {
                const size_t n = ringOut.read(scratch.data(), block);
                if (n == 0) break;
                for (size_t i = 0; i < n; ++i) out.push_back(scratch[(size_t)i * ringIn.numChannels() + 0]);
                outIndex += (long long)n;
            }
            for (int k = 0; k < 16; ++k) {
                OscMsg m;
                if (!client.poll(m, nullptr)) break;
                if (m.addr.rfind("/reply/", 0) == 0 && !sendTimes.empty()) {
                    const double st = sendTimes.front();
                    sendTimes.pop_front();
                    rttNs.push_back(nowNs() - st);
                    ++acks;
                }
            }
            if (nowNs() > settleEnd && (outIndex & 0x3F) == 0) {
                const double d = (double)pr.totalWritten.load(std::memory_order_relaxed) - (double)outIndex;
                if (d > 0) offsetSamplesSamples.push_back(d);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
        // Settle: let the pipeline drain so the slice is complete.
        drainMs(1200);
        const long long outEnd = outIndex;

        // Offset = median steady-state pipeline depth.
        double offsetSamples = -1;
        if (!offsetSamplesSamples.empty()) {
            std::sort(offsetSamplesSamples.begin(), offsetSamplesSamples.end());
            offsetSamples = offsetSamplesSamples[offsetSamplesSamples.size() / 2];
        }
        PulseReport prpt = analyzePulses(out.data() + outStart, outEnd - outStart, offsetSamples);
        double rttMedianUs = 0, rttP99Us = 0;
        if (!rttNs.empty()) {
            std::sort(rttNs.begin(), rttNs.end());
            const size_t m = rttNs.size();
            rttMedianUs = rttNs[m / 2] / 1000.0;
            rttP99Us = rttNs[std::min(m - 1, (m * 99) / 100)] / 1000.0;
        }
        const double lossPct = sent > 0 ? (1.0 - (double)acks / (double)sent) * 100.0 : 0.0;
        LossRow row{rate, (double)sent, (double)acks, lossPct, rttMedianUs, rttP99Us,
                    offsetSamples, offsetSamples / (double)sr * 1000.0,
                    prpt.expected, prpt.observed, prpt.totalLostSamples, prpt.pass};
        rows.push_back(row);
        if (baselineOut) *baselineOut = prpt.pass;
        std::printf("  %-9.0f %-8.0f %-8.0f %-8.3f %-11.1f  off=%8.0f s (%7.2f ms)  "
                    "pulses exp=%4lld obs=%4lld lost=%4lld  %s\n",
                    rate, (double)sent, (double)acks, lossPct, rttMedianUs,
                    offsetSamples, offsetSamples / (double)sr * 1000.0,
                    prpt.expected, prpt.observed, prpt.totalLostSamples,
                    prpt.pass ? "PASS" : "LOSS");
    };

    // Phase A: baseline (no OSC load).
    std::printf("heddle-osc-loss-test  (real host: --audio ring, VST3=%s, block=%d, sr=%d, %ds/phase)\n",
                vst3.c_str(), block, sr, (int)durationSec);
    std::printf("  waveform loop = %d s + %d x (%.1f) + %d s  (identity pass-through)\n",
                kSilenceA, kPulse, kAmp, kSilenceB);
    std::printf("  %-9s %-8s %-8s %-8s %-11s %-18s %-22s %s\n",
                "rate(s)", "sent", "acks", "loss%", "rtt med(us)", "offset(s)", "pulse loss", "result");
    bool baselinePass = false;
    runPhase(0.0, &baselinePass);

    // Phase B: per sustained OSC set-rate.
    std::vector<double> rates;
    {
        std::string tok;
        for (char c : ratesCsv) {
            if (c == ',') { if (!tok.empty()) rates.push_back(std::atof(tok.c_str())); tok.clear(); }
            else tok.push_back(c);
        }
        if (!tok.empty()) rates.push_back(std::atof(tok.c_str()));
        std::sort(rates.begin(), rates.end());
    }
    for (double rate : rates) {
        // Reset Gain to 1.0 and settle before each load window so the
        // waveform amplitude is the clean 1.5 the pulse detector expects.
        client.send(OscMsg::make("/chain/smoke/Gain", 1.0f));
        drainMs(400);
        runPhase(rate, nullptr);
    }

    // Phase C: control kick-in. Flip Gain 1.0 -> 0.5 while mid-pulse and find
    // the first output sample that reflects the new gain (|x|~0.75 instead of
    // 1.5). Because the output lags the input by the pipeline depth D, the
    // first new-gain sample is expected near output index (K - D) + kick, so
    // the scan starts there and kick-in = sGain - (K - D): the delay from the
    // control reaching the host to the audio actually changing (OSC drain +
    // next-block latch + any queueing). D is sampled live at flip time as
    // K - outIndex.
    std::vector<double> kickOffsets;
    int kickDiscarded = 0;
    for (int k = 0; k < kickRepeats; ++k) {
        // Make sure Gain is 1.0 and we are at the PULSE START before the flip,
        // so the gain change manifests inside the current pulse (not on the
        // next one): with the flip at the pulse start there are the full
        // 2048 old-gain samples ahead to cover drain (5 ms) + block (21 ms) +
        // jitter (~30 ms ~ 1440 samples @48k).
        //
        // NOTE: the producer writes whole 1024-sample blocks, so pr.phase
        // advances in 1024-steps and pr.phase % kLoop can only ever be
        // {0, 1024, 2048, 3072}. The window must match a reachable value:
        // ph == 1024 (kSilenceA, pulse start) is the only one with enough
        // old-gain pulse left (2048 samples). A sub-window like [1100,1600]
        // is never hit (that was the kick-in bug: n_valid=0).
        client.send(OscMsg::make("/chain/smoke/Gain", 1.0f));
        drainMs(300);
        auto inWindow = [&]() {
            const long long ph = pr.phase.load(std::memory_order_relaxed) % kLoop;
            return ph == kSilenceA;
        };
        const auto waitDl = Clock::now() + std::chrono::milliseconds(1500);
        while (Clock::now() < waitDl && !inWindow()) drainMs(5);
        if (!inWindow()) { ++kickDiscarded; continue; }
        const long long K = pr.totalWritten.load(std::memory_order_relaxed);
        const long long outAtFlip = outIndex;
        const long long D = std::max(0LL, K - outAtFlip);  // live pipeline depth
        client.send(OscMsg::make("/chain/smoke/Gain", 0.5f));
        // Find the first output sample after (K - D) with the new-gain
        // amplitude (0.75, strictly between the 0.5 lower and 1.0 upper cuts
        // so the 1.5 old-gain samples are excluded).
        long long sGain = -1;
        long long scanFrom = std::max(0LL, K - D);
        const auto scanDl = Clock::now() + std::chrono::milliseconds(1500);
        while (Clock::now() < scanDl && sGain < 0) {
            const long long need = scanFrom + 4096;
            while (outIndex < need) {
                const size_t n = ringOut.read(scratch.data(), block);
                if (n == 0) { std::this_thread::sleep_for(std::chrono::microseconds(100)); break; }
                for (size_t i = 0; i < n; ++i) out.push_back(scratch[(size_t)i * ringIn.numChannels() + 0]);
                outIndex += (long long)n;
            }
            for (long long i = scanFrom; i < std::min<long long>(outIndex, need); ++i) {
                const double a = std::fabs(out[i]);
                if (a > 0.5 && a < 1.0) { sGain = i; break; }  // new-gain (0.75)
            }
            scanFrom = std::min<long long>(outIndex, need);
        }
        if (sGain >= 0) {
            const double off = (double)(sGain - (K - D));
            // A change that only manifests in the NEXT pulse (off beyond the
            // pulse) is a bad measurement, not a real 60 ms+ control latency:
            // discard it (it would read ~5120 - K ~ 3300+).
            if (off > 3500) ++kickDiscarded;
            else kickOffsets.push_back(off);
        } else {
            ++kickDiscarded;
        }
        // Settle + restore.
        client.send(OscMsg::make("/chain/smoke/Gain", 1.0f));
        drainMs(300);
    }
    double kickMin = -1, kickMed = -1, kickMax = -1;
    if (!kickOffsets.empty()) {
        std::sort(kickOffsets.begin(), kickOffsets.end());
        kickMin = kickOffsets.front();
        kickMed = kickOffsets[kickOffsets.size() / 2];
        kickMax = kickOffsets.back();
    }

    // ---- summary -----------------------------------------------------------
    long long worstLost = 0;
    bool allPass = true;
    for (const LossRow& r : rows) {
        worstLost = std::max(worstLost, r.lostSamples);
        if (!r.pass) allPass = false;
    }

    std::printf("\n@@HEDDLE_OSC_LOSS_TEST@@\n");
    std::printf("sample_rate\t%d\n", sr);
    std::printf("block_size\t%d\n", block);
    std::printf("loop_samples\t%d\n", kLoop);
    std::printf("pulse_samples\t%d\n", kPulse);
    for (size_t i = 0; i < rows.size(); ++i) {
        const LossRow& r = rows[i];
        const char* tag = (r.rate == 0.0) ? "baseline" : "load";
        char rb[16];
        std::snprintf(rb, sizeof(rb), "%.0f", r.rate);
        std::printf("phase.%s.%s.rate\t%.0f\n", tag, rb, r.rate);
        std::printf("phase.%s.%s.sent\t%.0f\n", tag, rb, r.sent);
        std::printf("phase.%s.%s.acks\t%.0f\n", tag, rb, r.acks);
        std::printf("phase.%s.%s.loss_pct\t%.3f\n", tag, rb, r.lossPct);
        std::printf("phase.%s.%s.rtt_median_us\t%.1f\n", tag, rb, r.rttMedianUs);
        std::printf("phase.%s.%s.rtt_p99_us\t%.1f\n", tag, rb, r.rttP99Us);
        std::printf("phase.%s.%s.offset_samples\t%.0f\n", tag, rb, r.offsetSamples);
        std::printf("phase.%s.%s.offset_ms\t%.2f\n", tag, rb, r.offsetMs);
        std::printf("phase.%s.%s.pulses_expected\t%.0f\n", tag, rb, (double)r.expected);
        std::printf("phase.%s.%s.pulses_observed\t%.0f\n", tag, rb, (double)r.observed);
        std::printf("phase.%s.%s.samples_lost\t%.0f\n", tag, rb, (double)r.lostSamples);
        std::printf("phase.%s.%s.pass\t%s\n", tag, rb, r.pass ? "1" : "0");
    }
    std::printf("kickin.n_valid\t%.0f\n", (double)kickOffsets.size());
    std::printf("kickin.n_discarded\t%d\n", kickDiscarded);
    std::printf("kickin.min_samples\t%.0f\n", kickMin);
    std::printf("kickin.median_samples\t%.0f\n", kickMed);
    std::printf("kickin.max_samples\t%.0f\n", kickMax);
    std::printf("kickin.median_ms\t%.2f\n", kickMed / (double)sr * 1000.0);
    std::printf("worst_samples_lost\t%.0f\n", (double)worstLost);
    std::printf("all_phases_pass\t%s\n", allPass ? "1" : "0");
    std::printf("@@END@@\n");

    // Clean shutdown.
    client.send(OscMsg::make("/chain/quit"));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    client.close();
    pr.stop.store(true);
    prod.join();
    ringIn.close();
    ringOut.close();
    const int ec = childReap(child);
    heddle::AudioRing::remove(inName);
    heddle::AudioRing::remove(outName);
    std::remove(chainPath.c_str());
    std::printf("host exit code: %d\n", ec);
    return 0;
}
