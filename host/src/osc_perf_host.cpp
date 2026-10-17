// heddle-osc-perf-host -- OSC performance measured against a REAL live host.
//
// The host-level half of the OSC performance story (see docs/PERFORMANCE.md).
// The core-layer numbers (heddle_osc_perf) show the codec + classify/resolve/
// set path is microseconds and 4.7 Msets/s -- far from the limit. What a
// control client actually feels is this: a heddle-host running with a live
// audio-thread pump AND a VST3, driven over OSC. Two things shape that:
//
//   * The host drains OSC on a 5 ms message-thread timer (timerCallback does a
//     poll() until empty). A set therefore waits up to 5 ms for the next
//     drain, and sets arriving faster than the drain cadence queue in the
//     socket buffer. The set->ack RTT measured here includes that queueing,
//     the live VST3 setParam, and two UDP loopback hops -- the "realistic
//     latency" of one OSC control hop against a running host.
//
//   * The audio pump (ring mode) takes chainLock_ with a NON-blocking
//     try-lock; the OSC thread takes it BLOCKING around each setParam. Under
//     sustained set load the OSC thread holds the lock, the pump's try-lock
//     fails, and the pump skips ticks -- so the audio output throughput drops
//     and the input ring backlogs. That is the "smoothness of continuous osc
//     param adjustment" signal: how much real-time audio processing is
//     sacrificed while a client is turning a knob.
//
// For a ramp of sustained set-rates it measures:
//   - set->ack RTT (mean / median / p99 / max)
//   - UDP set loss  = 1 - acks/sent        -> max reliable rate
//   - audio output throughput (blocks/s) and its deficit vs the no-load
//     baseline, plus the peak input-ring backlog (frames)
//
// Links ONLY heddle_core (ring + OSC); the child heddle-host does the JUCE /
// audio / VST3 work. So like heddle-bridge-test it runs on a bare headless
// runner (no GUI, no audio device, no Xvfb). Exit 0 = all rates measured.
//
// Usage:
//   heddle-osc-perf-host <heddle-host> <smoke-vst3> [--rates 50,200,500,1000,2000,5000,10000]
//                        [--duration 3] [--block 1024] [--rate 48000]

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

// ---------------------------------------------------------------------------
// Child spawn/reap -- self-contained copy of the idiom from bridge_test.cpp.
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
        if (a.find_first_of(" \\\"") == std::string::npos) {
            cmd += a;
        } else {
            // MS command-line quoting: wrap in double quotes, double every
            // backslash, escape embedded quotes. (The older single-backslash
            // variant left a trailing backslash before the closing quote,
            // which the CRT parser reads as an escaped quote and swallows the
            // rest of the command line into argv[1].)
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
    return std::chrono::duration_cast<ClockNs>(Clock::now().time_since_epoch())
        .count();
}

struct RTTStats {
    double n = 0, mean = 0, median = 0, p99 = 0, max = 0;
    static RTTStats from(std::vector<double> ns) {
        RTTStats s;
        if (ns.empty()) return s;
        std::sort(ns.begin(), ns.end());
        const size_t m = ns.size();
        s.n = (double)m;
        s.max = ns.back();
        double sum = 0;
        for (double v : ns) sum += v;
        s.mean = sum / m;
        s.median = ns[m / 2];
        s.p99 = ns[std::min(m - 1, (m * 99) / 100)];
        return s;
    }
};

static double us(double ns) { return ns / 1000.0; }

// ---------------------------------------------------------------------------
// Audio producer: keeps the input ring topped up with a constant 0.5 signal so
// the host's pump is never input-starved. Runs on its own thread and reports
// how many frames it managed to write (the deficit vs. the pump's consumption
// is the backlog signal, sampled separately).
struct Producer {
    AudioRing* ring = nullptr;
    int ch = 2;
    int block = 1024;
    std::atomic<bool> stop{false};
    std::atomic<long long> written{0};
    std::vector<float> buf;
    void run() {
        buf.assign((size_t)block * ch, 0.5f);
        while (!stop.load()) {
            const size_t n = ring->write(buf.data(), block);
            if (n > 0) written.fetch_add((long long)n);
            else
                std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
};

// ---------------------------------------------------------------------------
static bool drain(AudioRing& out, float* scratch, int block, int ch,
                  long long* outFrames, int ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < deadline) {
        const size_t n = out.read(scratch, block);
        if (n > 0 && outFrames) *outFrames += (long long)n;
        else if (n == 0) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return true;
}

static void fail(ChildHandle& child, const std::string& chainPath,
                 const char* inName, const char* outName, const std::string& why) {
    std::fprintf(stderr, "FAIL: %s\n", why.c_str());
    childKill(child);
    if (child.valid) childReap(child);
    heddle::AudioRing::remove(inName);
    heddle::AudioRing::remove(outName);
    std::remove(chainPath.c_str());
    std::exit(1);
}

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
                     "usage: heddle-osc-perf-host <heddle-host> <smoke-vst3> "
                     "[--rates a,b,c] [--duration SEC] [--block N] [--rate SR]\n");
        return 2;
    }
    const std::string hostExe = argv[1];
    const std::string vst3 = argv[2];
    std::string ratesCsv = "50,200,500,1000,2000,5000,10000";
    double durationSec = 3.0;
    int block = 1024;
    int sr = 48000;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };
        if (a == "--rates") ratesCsv = next();
        else if (a == "--duration") durationSec = std::atof(next());
        else if (a == "--block") block = std::atoi(next());
        else if (a == "--rate") sr = std::atoi(next());
    }

    const char* inName = "heddle.perf.in";
    const char* outName = "heddle.perf.out";
    long seed;
#if defined(_WIN32)
    seed = (long)GetCurrentProcessId();
#else
    seed = (long)getpid();
#endif
    const int hostPort = 21000 + (int)(seed % 4000);

    const std::string chainPath = dirOf(hostExe) + "heddle_osc_perf_host_chain.json";
    ChildHandle child;
    {
        std::string json =
            "{\"sample_rate\": " + std::to_string(sr) + ", \"block_size\": " +
            std::to_string(block) + ", \"chain\": [ { \"id\": \"smoke\", "
            "\"plugin\": \"" + jsonEscape(vst3) + "\", \"enabled\": true, "
            "\"params\": { \"Gain\": 0.5, \"PWM pct\": 1.0 } } ] }\n";
        FILE* f = std::fopen(chainPath.c_str(), "wb");
        if (!f) fail(child, chainPath, inName, outName, "cannot write chain: " + chainPath);
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
    }

    heddle::AudioRing::remove(inName);
    heddle::AudioRing::remove(outName);

    std::vector<std::string> hargv = {hostExe, chainPath, "--audio", "ring",
                                      "--ring", "heddle.perf",
                                      "--osc-port", std::to_string(hostPort)};
    child = spawnChild(hostExe, hargv);
    if (!child.valid) fail(child, chainPath, inName, outName, "failed to spawn heddle-host");

    const auto ringDeadline = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < ringDeadline &&
           !(heddle::AudioRing::exists(inName) && heddle::AudioRing::exists(outName))) {
        if (childFinished(child))
            fail(child, chainPath, inName, outName, "heddle-host exited before creating rings");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!(heddle::AudioRing::exists(inName) && heddle::AudioRing::exists(outName)))
        fail(child, chainPath, inName, outName, "rings did not appear within 30 s");

    std::string err;
    AudioRing ringIn = AudioRing::open(inName, &err, 3000);
    AudioRing ringOut = AudioRing::open(outName, &err, 3000);
    if (!ringIn.valid() || !ringOut.valid())
        fail(child, chainPath, inName, outName, "open rings failed: " + err);

    // OSC client (full duplex: send sets + receive acks).
    OscUdp client;
    if (!client.bind("127.0.0.1", 0, &err))
        fail(child, chainPath, inName, outName, "client bind failed: " + err);
    if (!client.openOut("127.0.0.1", hostPort, &err))
        fail(child, chainPath, inName, outName, "client openOut failed: " + err);

    // Verify the host is alive and has the slot.
    client.send(OscMsg::make("/chain/query"));
    bool gotChain = false;
    std::string chainBody;
    {
        OscMsg m;
        const auto dl = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < dl && !gotChain) {
            if (client.poll(m) && m.addr == "/reply/chain" && !m.strs.empty()) {
                gotChain = true;
                chainBody = m.strs[0];
            } else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (!gotChain) fail(child, chainPath, inName, outName, "no /reply/chain for /chain/query");
    if (chainBody.find("smoke") == std::string::npos)
        fail(child, chainPath, inName, outName, "/reply/chain missing 'smoke': " + chainBody);

    // Warm up the audio path (fill the input ring + let the pump produce a
    // little) so the first measured window is steady state.
    {
        std::vector<float> fill((size_t)block * ringIn.numChannels(), 0.5f);
        for (int i = 0; i < 256; ++i) {
            const size_t w = ringIn.write(fill.data(), block);
            (void)w;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        // Drain accumulated output + backlog to a clean baseline.
        std::vector<float> scr((size_t)block * ringIn.numChannels(), 0.0f);
        drain(ringOut, scr.data(), block, ringIn.numChannels(), nullptr, 1500);
    }

    // Baseline audio throughput with NO OSC load (the smoothness reference).
    double baselineBlocksPerSec = 0;
    {
        Producer pr;
        pr.ring = &ringIn;
        pr.ch = ringIn.numChannels();
        pr.block = block;
        std::thread pt([&]() { pr.run(); });
        std::vector<float> scr((size_t)block * ringIn.numChannels(), 0.0f);
        const double t0 = nowNs();
        long long baseFrames = 0;
        drain(ringOut, scr.data(), block, ringIn.numChannels(), &baseFrames,
              (int)(durationSec * 1000));
        const double t1 = nowNs();
        pr.stop.store(true);
        pt.join();
        baselineBlocksPerSec = (baseFrames / (double)block) / ((t1 - t0) / 1e9);
        // Settle.
        drain(ringOut, scr.data(), block, ringIn.numChannels(), nullptr, 1500);
    }

    // Parse the rate list.
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

    std::printf("heddle-osc-perf-host  (real host: --audio ring, VST3=%s, block=%d, sr=%d, %ds/rate)\n",
                vst3.c_str(), block, sr, (int)durationSec);
    std::printf("  baseline audio throughput (no OSC load): %.1f blocks/s\n", baselineBlocksPerSec);
    std::printf("  %-9s %-9s %-9s %-12s %-12s %-10s %-10s %-10s\n",
                "rate(s)", "sent", "acks", "loss(%)", "rtt med(us)", "rtt p99(us)",
                "blk/s", "deficit(%)");

    struct Row {
        double rate, sent, acks, lossPct, rttMedianUs, rttP99Us, blkPerSec, deficitPct, peakBacklog;
    };
    std::vector<Row> rows;

    for (double rate : rates) {
        Producer pr;
        pr.ring = &ringIn;
        pr.ch = ringIn.numChannels();
        pr.block = block;
        pr.written.store(0);
        std::thread pt([&]() { pr.run(); });
        std::vector<float> scr((size_t)block * ringIn.numChannels(), 0.0f);

        // Settle: let the backlog drain before measuring this rate.
        drain(ringOut, scr.data(), block, ringIn.numChannels(), nullptr, 800);
        // Drain any stale OSC acks.
        {
            OscMsg m;
            while (client.poll(m, nullptr)) {
            }
        }

        const double periodNs = 1e9 / rate;
        const double t0 = nowNs();
        const double t1 = t0 + durationSec * 1e9;
        long long sent = 0;
        long long acks = 0;
        long long outFrames = 0;
        long long peakBacklog = 0;
        double nextSend = t0;
        std::deque<double> sendTimes;  // FIFO: oldest un-acked set's send time
        std::vector<double> rttNs;
        rttNs.reserve(1 << 16);

        while (nowNs() < t1) {
            // Send any due sets.
            while (nowNs() >= nextSend && nextSend < t1) {
                client.send(OscMsg::make("/chain/smoke/Gain", 0.5f));
                sendTimes.push_back(nextSend);
                ++sent;
                nextSend += periodNs;
            }
            // Read output (count throughput).
            for (int k = 0; k < 16; ++k) {
                const size_t n = ringOut.read(scr.data(), block);
                if (n > 0) outFrames += (long long)n;
                else break;
            }
            // Sample backlog.
            const long long backlog = (long long)ringIn.dataAvailable();
            if (backlog > peakBacklog) peakBacklog = backlog;
            // Receive acks.
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
            // Tiny sleep to keep the loop off 100% CPU without adding skew.
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
        const double elapsed = (nowNs() - t0) / 1e9;
        pr.stop.store(true);
        pt.join();
        // Settle after.
        drain(ringOut, scr.data(), block, ringIn.numChannels(), nullptr, 1200);

        const RTTStats rtt = RTTStats::from(rttNs);
        const double blkPerSec = (outFrames / (double)block) / elapsed;
        const double deficitPct = (baselineBlocksPerSec > 0)
            ? (1.0 - blkPerSec / baselineBlocksPerSec) * 100.0
            : 0.0;
        const double lossPct = (sent > 0) ? (1.0 - (double)acks / (double)sent) * 100.0 : 0.0;

        Row row{rate, (double)sent, (double)acks, lossPct, us(rtt.median), us(rtt.p99),
                blkPerSec, deficitPct, (double)peakBacklog};
        rows.push_back(row);
        std::printf("  %-9.0f %-9.0f %-9.0f %-12.3f %-12.1f %-10.1f %-10.1f %-10.1f\n",
                    rate, (double)sent, (double)acks, lossPct, us(rtt.median), us(rtt.p99),
                    blkPerSec, deficitPct);
    }

    // Max reliable rate: highest tested rate with <1% set loss AND <10% audio
    // deficit. (Both thresholds are reported above; this is the headline.)
    double maxReliable = 0;
    for (const Row& r : rows)
        if (r.lossPct < 1.0 && r.deficitPct < 10.0) maxReliable = r.rate;

    // Machine-readable block.
    std::printf("\n@@HEDDLE_OSC_PERF_HOST@@\n");
    std::printf("baseline.blocks_per_s\t%.1f\n", baselineBlocksPerSec);
    for (const Row& r : rows) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f", r.rate);
        std::printf("rate.%s.sent\t%.0f\n", buf, r.sent);
        std::printf("rate.%s.acks\t%.0f\n", buf, r.acks);
        std::printf("rate.%s.loss_pct\t%.3f\n", buf, r.lossPct);
        std::printf("rate.%s.rtt_median_us\t%.1f\n", buf, r.rttMedianUs);
        std::printf("rate.%s.rtt_p99_us\t%.1f\n", buf, r.rttP99Us);
        std::printf("rate.%s.blocks_per_s\t%.1f\n", buf, r.blkPerSec);
        std::printf("rate.%s.audio_deficit_pct\t%.1f\n", buf, r.deficitPct);
        std::printf("rate.%s.peak_input_backlog_frames\t%.0f\n", buf, r.peakBacklog);
    }
    std::printf("max_reliable_rate_sets_per_s\t%.0f\n", maxReliable);
    std::printf("@@END@@\n");

    // Clean shutdown.
    client.send(OscMsg::make("/chain/quit"));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    client.close();
    ringIn.close();
    ringOut.close();
    const int ec = childReap(child);
    heddle::AudioRing::remove(inName);
    heddle::AudioRing::remove(outName);
    std::remove(chainPath.c_str());
    std::printf("host exit code: %d\n", ec);
    return 0;
}
