// heddle-osc-ramp-test -- SAMPLE-LEVEL accuracy test for in-block OSC curve
// scheduling (VST3 IParamValueQueue model over UDP), against a REAL live host.
//
// WHY THIS EXISTS
//   heddle-osc-loss-test proves no *samples* are lost under OSC load. This
//   tool proves the stronger, different claim: when the user sweeps a param
//   (gain) via OSC, the rendered output follows the commanded ramp, and the
//   residual error is bounded by the sub-block staircase (segment size) --
//   NOT by ring drops, poll jitter, or the OSC rate (as long as the rate is
//   >= ~1 set/block). It answers, with numbers:
//     - "is 200 sets/s enough?"
//     - "what is the critical OSC rate?"  (error floor vs rate)
//     - "how does block size matter?"     (error floor vs block size)
//
// THE SIGNAL (per spec)
//   Input : 40960-sample window (>= 10 blocks even at block=2048) of
//           constant +1.0.
//   Control: the smoke plugin's Gain (id "gain", 0..1) is swept 0.05 -> 1.00
//            by a ramp of OSC waypoints spread evenly across the window. The
//            sweep rate is specified in SETS PER BLOCK (ppb) -- the natural
//            unit for in-block scheduling (0.1, 1, 10, 40 by default).
//   Plugin: Gain=1.0 / PWM=1.0 -> out = in * Gain * 1 = Gain exactly.
//
// THE REFERENCE CURVE (robust, no host-internals prediction)
//   The commanded ramp is a straight line in host frames (0.05 -> 1.00 across
//   the window). The measured output approximates that line with a staircase:
//     - inblock: sub-block staircase, step = segment size (substep)
//     - legacy : per-block staircase (model latched, applied at next block)
//   We LEAST-SQUARES FIT A STRAIGHT LINE to the measured output over the ramp
//   region and report the deviation from that line as the scheduling fidelity
//   floor. The line fit absorbs pipeline depth, poll phase, and the 2-block
//   lookahead automatically -- so no nominal-curve prediction (and no sign
//   errors) is needed. The residual IS the error we care about.
//
// WHAT IT MEASURES (per run = one host, one (blockSize, ppb, mode))
//   A. max / rms / mean|error| of (measured - fitted line) over the ramp
//      region -- the fidelity floor.
//   B. R^2 of the line fit (how well the output follows a straight ramp).
//   C. OFFSET: steady-state pipeline depth D = totalWritten - outIndex
//      (samples + ms) -- the round-trip latency floor.
//   D. KICK-IN: first output sample to drop below 0.90; residual = that index
//      minus (D + 2*block) -- the DAW "knob feels instant" number.
//   E. Loss proxy: samples where |out| < 0.02 (a ramp value that should be
//      >= 0.05) -- any such sample means the chain dropped or zeroed audio.
//   F. sets sent / acked / loss% + set->ack RTT median/p99 (UDP sanity).
//
// MODES
//   default : in-block (the new sample-accurate curve scheduler).
//   --legacy: spawn the host with --no-inblock (immediate per-block latch) so
//             the infographic can show what the old path costs at each rate.
//
// LINKS ONLY heddle_core (ring + OSC); the child heddle-host does the JUCE /
// audio / VST3 work. Runs headless (no GUI, no audio device).
//
// USAGE
//   heddle-osc-ramp-test <heddle-host> <smoke-vst3> [--legacy]
//       [--blocks 128,256,512,1024,2048] [--ppb 0.1,1,10,40]
//       [--rate 48000] [--substep 4] [--settle 1500]
//
// Exit 0 = all runs completed (errors are REPORTED, not hard-failed).
// Exit 1 = a run errored (ring never appeared, host died, etc.).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
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
using ClockNs = std::chrono::nanoseconds;

// ---- constants -------------------------------------------------------------
static constexpr long long kWinSamples = 40960;   // the 40960-sample window
static constexpr double    kGainLo = 0.05;        // ramp start (measurable dip)
static constexpr double    kGainHi = 1.00;        // ramp end
static constexpr double    kLossFloor = 0.02;     // |out| below this = lost sample
static constexpr double    kKickThreshold = 0.90; // first sample < this = ramp started
static constexpr size_t    kRingCap = 512;        // host in-block ring capacity

static double nowNs() {
    return std::chrono::duration_cast<ClockNs>(Clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Child spawn/reap -- same idiom as osc_loss_test.cpp.
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
                cmd += '\\';
                if (c == '"') cmd.push_back('\\');
                cmd.push_back(c);
            }
            cmd += '"';
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
// Producer: streams constant +1.0 into the input ring, real-time paced
// (same backpressure idiom as the loss test). totalWritten is the host INPUT
// frame count -- the same-clock anchor for the sweep.
struct Producer {
    AudioRing* ring = nullptr;
    int ch = 2;
    int block = 1024;
    int sr = 48000;
    std::atomic<bool> stop{false};
    std::atomic<long long> totalWritten{0};
    std::vector<float> buf;
    void run() {
        buf.assign((size_t)block * ch, 1.0f);
        auto target = Clock::now();
        while (!stop.load()) {
            const size_t n = ring->write(buf.data(), block);
            if (n > 0) {
                totalWritten.fetch_add((long long)n, std::memory_order_relaxed);
                target += std::chrono::duration_cast<ClockNs>(
                    std::chrono::duration<double, std::ratio<1, 1000000000>>(
                        (double)n * 1e9 / (double)sr));
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
struct RunResult {
    int block;
    bool legacy;
    double ppb;          // sets per block (sweep axis)
    double rateSps;      // sets/s at this block
    long long nSends;
    long long nAcks;
    double lossPct;
    double rttMedianUs, rttP99Us;
    double offsetSamples, offsetMs;   // pipeline depth D
    double kickinSamples, kickinMs;   // vs (D + 2B)
    double maxErr, rmsErr, mae;
    double r2;                    // line-fit goodness (how linear the ramp is)
    double fitSlope, idealSlope;  // fitted vs ideal ramp slope
    long long lostSamples;
    long long nSamples;           // ramp-region length used for the fit
    bool ok;
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
static std::vector<long long> parseLL(const std::string& csv, const std::vector<long long>& def) {
    std::vector<long long> out;
    std::string tok;
    for (char c : csv) {
        if (c == ',') { if (!tok.empty()) { out.push_back(std::atoll(tok.c_str())); tok.clear(); } }
        else tok.push_back(c);
    }
    if (!tok.empty()) out.push_back(std::atoll(tok.c_str()));
    return out.empty() ? def : out;
}
static std::vector<double> parseD(const std::string& csv, const std::vector<double>& def) {
    std::vector<double> out;
    std::string tok;
    for (char c : csv) {
        if (c == ',') { if (!tok.empty()) { out.push_back(std::atof(tok.c_str())); tok.clear(); } }
        else tok.push_back(c);
    }
    if (!tok.empty()) out.push_back(std::atof(tok.c_str()));
    return out.empty() ? def : out;
}

// Least-squares line fit y = a + b*x over [a0, a1) of `y`; returns (a, b) and
// the coefficient of determination R^2. Fills nothing else.
static void fitLine(const std::vector<float>& y, long long a0, long long a1,
                    double& a, double& b, double& r2) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
    long long n = 0;
    for (long long i = a0; i < a1; ++i) {
        const double x = (double)i;
        const double v = (double)y[i];
        sx += x; sy += v; sxx += x * x; sxy += x * v; syy += v * v;
        ++n;
    }
    const double denom = (double)n * sxx - sx * sx;
    if (denom == 0.0) { a = (n ? sy / (double)n : 0.0); b = 0.0; r2 = 0.0; return; }
    b = ((double)n * sxy - sx * sy) / denom;
    a = (sy - b * sx) / (double)n;
    // R^2 = 1 - SS_res/SS_tot
    double ssRes = 0, ssTot = 0;
    const double ybar = sy / (double)n;
    for (long long i = a0; i < a1; ++i) {
        const double v = (double)y[i];
        const double e = v - (a + b * (double)i);
        ssRes += e * e;
        ssTot += (v - ybar) * (v - ybar);
    }
    r2 = (ssTot > 0.0) ? (1.0 - ssRes / ssTot) : 0.0;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: heddle-osc-ramp-test <heddle-host> <smoke-vst3> [--legacy]\n"
            "       [--blocks 128,256,512,1024,2048] [--ppb 0.1,1,10,40]\n"
            "       [--rate 48000] [--substep 4] [--settle 1500]\n");
        return 2;
    }
    const std::string hostExe = argv[1];
    const std::string vst3 = argv[2];
    std::string blocksCsv = "128,256,512,1024,2048";
    std::string ppbCsv = "0.1,1,10,40";
    int sr = 48000;
    int substep = 4;
    int settleMs = 1500;
    bool legacy = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };
        if (a == "--blocks") blocksCsv = next();
        else if (a == "--ppb") ppbCsv = next();
        else if (a == "--rate") sr = std::atoi(next());
        else if (a == "--substep") substep = std::atoi(next());
        else if (a == "--settle") settleMs = std::atoi(next());
        else if (a == "--legacy") legacy = true;
    }
    std::vector<long long> blocks = parseLL(blocksCsv, {128, 256, 512, 1024, 2048});
    std::vector<double> ppbs = parseD(ppbCsv, {0.1, 1.0, 10.0, 40.0});
    std::sort(blocks.begin(), blocks.end());
    std::sort(ppbs.begin(), ppbs.end());
    for (long long b : blocks)
        for (double p : ppbs)
            if (p > (double)(kRingCap / 2))
                std::fprintf(stderr, "WARN: ppb=%.1f @block=%lld may overflow the %zu ring\n",
                             p, b, kRingCap);

    std::vector<RunResult> results;
    int hostPortBase = 21900 + (int)((long)std::hash<std::string>()(hostExe) % 300);

    for (long long block : blocks) {
        const int hostPort = hostPortBase + (int)(block % 200);
        const std::string chainPath = dirOf(hostExe) + "heddle_osc_ramp_test_chain.json";
        {
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
        const std::string ringPrefix = "heddle.ramp";
        heddle::AudioRing::remove(ringPrefix + ".in");
        heddle::AudioRing::remove(ringPrefix + ".out");
        std::vector<std::string> hargv = {hostExe, chainPath, "--audio", "ring",
                                          "--ring", ringPrefix,
                                          "--osc-port", std::to_string(hostPort)};
        if (legacy) hargv.push_back("--no-inblock");
        hargv.push_back("--substep");
        hargv.push_back(std::to_string(substep));
        ChildHandle child = spawnChild(hostExe, hargv);
        if (!child.valid) { std::fprintf(stderr, "FAIL: spawn heddle-host (block=%lld)\n", block); return 1; }
        const auto ringDeadline = Clock::now() + std::chrono::seconds(30);
        while (Clock::now() < ringDeadline &&
               !(heddle::AudioRing::exists(ringPrefix + ".in") &&
                 heddle::AudioRing::exists(ringPrefix + ".out"))) {
            if (childFinished(child)) { std::fprintf(stderr, "FAIL: host exited before rings (block=%lld)\n", block); childKill(child); return 1; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!(heddle::AudioRing::exists(ringPrefix + ".in") &&
              heddle::AudioRing::exists(ringPrefix + ".out"))) {
            std::fprintf(stderr, "FAIL: rings did not appear (block=%lld)\n", block);
            childKill(child); childReap(child); return 1;
        }
        std::string err;
        AudioRing ringIn = AudioRing::open(ringPrefix + ".in", &err, 3000);
        AudioRing ringOut = AudioRing::open(ringPrefix + ".out", &err, 3000);
        if (!ringIn.valid() || !ringOut.valid()) {
            std::fprintf(stderr, "FAIL: open rings: %s\n", err.c_str());
            childKill(child); childReap(child); return 1;
        }
        OscUdp client;
        if (!client.bind("127.0.0.1", 0, &err)) { std::fprintf(stderr, "FAIL: client bind: %s\n", err.c_str()); childKill(child); childReap(child); return 1; }
        if (!client.openOut("127.0.0.1", hostPort, &err)) { std::fprintf(stderr, "FAIL: client openOut: %s\n", err.c_str()); childKill(child); childReap(child); return 1; }
        client.send(OscMsg::make("/chain/query"));
        {
            OscMsg m;
            const auto dl = Clock::now() + std::chrono::seconds(5);
            bool got = false;
            while (Clock::now() < dl && !got) {
                if (client.poll(m) && m.addr == "/reply/chain" && !m.strs.empty()) got = true;
                else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!got) { std::fprintf(stderr, "FAIL: no /reply/chain (block=%lld)\n", block); childKill(child); childReap(child); return 1; }
        }

        const int nCh = ringIn.numChannels();
        std::vector<float> scratch((size_t)block * nCh, 0.0f);
        Producer pr;
        pr.ring = &ringIn;
        pr.ch = nCh;
        pr.block = (int)block;
        pr.sr = sr;
        pr.totalWritten.store(0);
        std::thread prod([&]() { pr.run(); });

        // Warm up to steady state; sanity-check the pass-through is ~1.0.
        long long outIndex = 0;
        float warmupMax = 0.f;
        {
            const auto dl = Clock::now() + std::chrono::milliseconds(settleMs);
            while (Clock::now() < dl) {
                const size_t n = ringOut.read(scratch.data(), block);
                if (n > 0) {
                    outIndex += (long long)n;
                    for (size_t i = 0; i < n; ++i)
                        if (scratch[(size_t)i * nCh + 0] > warmupMax)
                            warmupMax = scratch[(size_t)i * nCh + 0];
                } else std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        const double warmupD = (double)pr.totalWritten.load(std::memory_order_relaxed) - (double)outIndex;
        std::fprintf(stderr, "[block=%lld %s] warmup D=%.0f s (=%.2f ms) outMax=%.3f\n", block,
                     legacy ? "legacy" : "inblock", warmupD, warmupD / (double)sr * 1000.0, warmupMax);
        if (warmupMax < 0.95f)
            std::fprintf(stderr, "WARN: pass-through not ~1.0 (max=%.3f); results suspect\n", warmupMax);

        // ---- ppb sweep on this host ----------------------------------------
        for (double ppb : ppbs) {
            const double rateSps = ppb * (double)sr / (double)block;
            const int nSends = (int)std::llround(ppb * (double)kWinSamples / (double)block);
            if (nSends < 8) continue;
            const double period = (double)kWinSamples / (double)(nSends - 1);
            const double periodNs = period * 1e9 / (double)sr;
            const double idealSlope = (kGainHi - kGainLo) / (double)kWinSamples;

            // Reset gain to 1.0 and settle so the pre-ramp output is a clean 1.0.
            client.send(OscMsg::make("/chain/smoke/Gain", 1.0f));
            {
                const auto dl = Clock::now() + std::chrono::milliseconds(400);
                while (Clock::now() < dl) {
                    const size_t n = ringOut.read(scratch.data(), block);
                    if (n > 0) outIndex += (long long)n;
                    else std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
            const long long twStart = pr.totalWritten.load(std::memory_order_relaxed);
            const double D0 = (double)pr.totalWritten.load(std::memory_order_relaxed) - (double)outIndex;

            // Capture length: ramp (kWinSamples) + kick-in lead (D0+2B) + tail.
            const long long Wcap = kWinSamples + (long long)(D0 + 4.0 * block) + 4096;

            std::vector<float> win;
            win.reserve((size_t)Wcap);
            std::vector<double> rttNs;
            std::deque<double> sendTimes;
            long long acks = 0;

            const double t0 = nowNs();
            long long j = 0;
            const auto hardDeadline = Clock::now() +
                std::chrono::milliseconds((int)((Wcap * 1e9 / (double)sr) / 1e6) + 5000);
            while ((j < nSends || (long long)win.size() < Wcap) &&
                   Clock::now() < hardDeadline) {
                bool progressed = false;
                // 1. Read output as fast as possible (never let the ring overflow).
                const size_t n = ringOut.read(scratch.data(), block);
                if (n > 0) {
                    for (size_t i = 0; i < n; ++i)
                        win.push_back(scratch[(size_t)i * nCh + 0]);
                    outIndex += (long long)n;
                    progressed = true;
                }
                // 2. Drain acks (RTT stats + socket hygiene).
                OscMsg m;
                while (client.poll(m, nullptr)) {
                    if (m.addr.rfind("/reply/", 0) == 0 && !sendTimes.empty()) {
                        rttNs.push_back(nowNs() - sendTimes.front());
                        sendTimes.pop_front();
                        ++acks;
                    }
                    progressed = true;
                }
                // 3. Send every ramp waypoint that is due by now (paced; small
                //    bursts when the period is sub-tick, which is correct -- a
                //    DAW automation emits at its own cadence, not per sample).
                const double now = nowNs();
                while (j < nSends && (t0 + (double)j * periodNs) <= now) {
                    const double frac = (double)j / (double)(nSends - 1);
                    const double val = kGainLo + (kGainHi - kGainLo) * frac;
                    client.send(OscMsg::make("/chain/smoke/Gain", (float)val));
                    sendTimes.push_back(now);
                    ++j;
                    progressed = true;
                }
                if (!progressed)
                    std::this_thread::sleep_for(std::chrono::microseconds(20));
            }
            const double D1 = (double)pr.totalWritten.load(std::memory_order_relaxed) - (double)outIndex;

            const long long W = std::min<long long>((long long)win.size(), Wcap);

            // Kick-in: first window sample to drop below the threshold.
            long long iChange = -1;
            for (long long i = 0; i < W; ++i)
                if ((double)win[i] < kKickThreshold) { iChange = i; break; }

            // Ramp region: the ramp spans kWinSamples (in host frames == window
            // indices, modulo the constant offset that the fit absorbs).
            const long long margin = std::max(2LL * block, 256LL);
            long long cA = 0, cB = 0;
            if (iChange >= 0) {
                cA = iChange + margin;
                cB = iChange + kWinSamples - margin;
                if (cB > W) cB = W;
                if (cA < margin) cA = margin;
            }
            // Fallback: if the kick-in wasn't found (shouldn't happen), use the
            // middle of the window.
            if (iChange < 0 || cB - cA < (long long)block) {
                cA = std::max(0LL, (long long)(W * 0.25));
                cB = std::min(W, (long long)(W * 0.75));
            }

            double a = 0, b = 0, r2 = 0;
            fitLine(win, cA, cB, a, b, r2);

            double sq = 0, absSum = 0, maxErr = 0;
            long long nC = 0, lost = 0;
            for (long long i = 0; i < W; ++i) {
                const double v = (double)win[i];
                if (std::fabs(v) < kLossFloor) ++lost;
                if (i < cA || i >= cB) continue;
                const double e = v - (a + b * (double)i);
                const double ae = std::fabs(e);
                if (ae > maxErr) maxErr = ae;
                sq += e * e;
                absSum += ae;
                ++nC;
            }
            const double rms = (nC > 0) ? std::sqrt(sq / (double)nC) : -1;
            const double mae = (nC > 0) ? absSum / (double)nC : -1;

            double kickin = -1;
            if (iChange >= 0)
                kickin = (double)iChange - (D0 + 2.0 * block);

            double rttMedianUs = 0, rttP99Us = 0;
            if (!rttNs.empty()) {
                std::sort(rttNs.begin(), rttNs.end());
                const size_t mm = rttNs.size();
                rttMedianUs = rttNs[mm / 2] / 1000.0;
                rttP99Us = rttNs[std::min(mm - 1, (mm * 99) / 100)] / 1000.0;
            }

            // Optional diagnostic dump of the measured window (env-gated).
            if (const char* dumpDir = std::getenv("HEDDLE_RAMP_DUMP")) {
                if (dumpDir[0] != '\0') {
                    std::string fn = std::string(dumpDir) + "/ramp_" +
                        std::to_string((int)block) + "_" +
                        std::to_string((long long)std::llround(ppb * 10)) +
                        (legacy ? "_legacy.txt" : ".txt");
                    FILE* f = std::fopen(fn.c_str(), "wb");
                    if (f) {
                        std::fprintf(f, "# block=%lld ppb=%.3f substep=%d a=%.10g b=%.10g r2=%.6g D0=%.0f iChange=%lld cA=%lld cB=%lld\n",
                                     block, ppb, substep, a, b, r2, D0, iChange, cA, cB);
                        for (long long i = 0; i < W; ++i)
                            std::fprintf(f, "%.9g\n", (double)win[i]);
                        std::fclose(f);
                    }
                }
            }

            const double offsetSamples = (D0 + D1) * 0.5;
            RunResult r;
            r.block = (int)block;
            r.legacy = legacy;
            r.ppb = ppb;
            r.rateSps = rateSps;
            r.nSends = nSends;
            r.nAcks = acks;
            r.lossPct = (nSends > 0) ? (1.0 - (double)acks / (double)nSends) * 100.0 : 0.0;
            r.rttMedianUs = rttMedianUs;
            r.rttP99Us = rttP99Us;
            r.offsetSamples = offsetSamples;
            r.offsetMs = offsetSamples / (double)sr * 1000.0;
            r.kickinSamples = kickin;
            r.kickinMs = kickin / (double)sr * 1000.0;
            r.maxErr = maxErr;
            r.rmsErr = rms;
            r.mae = mae;
            r.r2 = r2;
            r.fitSlope = b;
            r.idealSlope = idealSlope;
            r.lostSamples = lost;
            r.nSamples = nC;
            r.ok = (iChange >= 0) && (W >= (long long)kWinSamples * 0.98) &&
                   (nC > (long long)block) && (std::fabs(b) > 0.5 * std::fabs(idealSlope));
            results.push_back(r);

            std::printf("  block=%-5lld %-7s ppb=%6.1f  %9.0f/s  D=%7.0f s (%6.2f ms)  kickin=%8.1f s  "
                        "max=%9.6f  rms=%9.6f  mae=%9.6f  R2=%7.5f  slope=%8.3e/%8.3e  lost=%5lld  acks=%5lld/%-5d  %s\n",
                        block, legacy ? "legacy" : "inblock", ppb, rateSps,
                        offsetSamples, r.offsetMs, kickin, maxErr, rms, mae, r2,
                        b, idealSlope, lost, acks, nSends, r.ok ? "ok" : "ERR");
        }

        // Tear down this host.
        client.send(OscMsg::make("/chain/quit"));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        client.close();
        pr.stop.store(true);
        prod.join();
        ringIn.close();
        ringOut.close();
        childReap(child);
        heddle::AudioRing::remove(ringPrefix + ".in");
        heddle::AudioRing::remove(ringPrefix + ".out");
        std::remove(chainPath.c_str());
    }

    // ---- summary table ------------------------------------------------------
    std::printf("\n@@HEDDLE_OSC_RAMP_TEST@@\n");
    std::printf("sample_rate\t%d\n", sr);
    std::printf("window_samples\t%lld\n", (long long)kWinSamples);
    std::printf("substep\t%d\n", substep);
    for (size_t i = 0; i < results.size(); ++i) {
        const RunResult& r = results[i];
        std::printf("run.%d.mode\t%s\n", (int)i, r.legacy ? "legacy" : "inblock");
        std::printf("run.%d.block\t%d\n", (int)i, r.block);
        std::printf("run.%d.ppb\t%.3f\n", (int)i, r.ppb);
        std::printf("run.%d.rate_sps\t%.1f\n", (int)i, r.rateSps);
        std::printf("run.%d.sets_sent\t%.0f\n", (int)i, (double)r.nSends);
        std::printf("run.%d.sets_acked\t%.0f\n", (int)i, (double)r.nAcks);
        std::printf("run.%d.set_loss_pct\t%.3f\n", (int)i, r.lossPct);
        std::printf("run.%d.rtt_median_us\t%.1f\n", (int)i, r.rttMedianUs);
        std::printf("run.%d.rtt_p99_us\t%.1f\n", (int)i, r.rttP99Us);
        std::printf("run.%d.offset_samples\t%.0f\n", (int)i, r.offsetSamples);
        std::printf("run.%d.offset_ms\t%.3f\n", (int)i, r.offsetMs);
        std::printf("run.%d.kickin_samples\t%.1f\n", (int)i, r.kickinSamples);
        std::printf("run.%d.kickin_ms\t%.3f\n", (int)i, r.kickinMs);
        std::printf("run.%d.max_err\t%.6f\n", (int)i, r.maxErr);
        std::printf("run.%d.rms_err\t%.6f\n", (int)i, r.rmsErr);
        std::printf("run.%d.mae\t%.6f\n", (int)i, r.mae);
        std::printf("run.%d.r2\t%.5f\n", (int)i, r.r2);
        std::printf("run.%d.fit_slope\t%.6e\n", (int)i, r.fitSlope);
        std::printf("run.%d.ideal_slope\t%.6e\n", (int)i, r.idealSlope);
        std::printf("run.%d.lost_samples\t%.0f\n", (int)i, (double)r.lostSamples);
        std::printf("run.%d.ramp_samples\t%.0f\n", (int)i, (double)r.nSamples);
        std::printf("run.%d.ok\t%s\n", (int)i, r.ok ? "1" : "0");
    }
    std::printf("@@END@@\n");
    return 0;
}
