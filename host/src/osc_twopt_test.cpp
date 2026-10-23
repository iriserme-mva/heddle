// heddle-osc-twopt -- TWO-WAYPOINT in-block curve verification against a REAL
// live host (block size default 512).
//
// WHAT IT PROVES
//   The user question: "two OSC values (0 and 1) map to two blocks inside the
//   window -- where exactly do the values land, and what happens in between?"
//   The host's in-block scheduler turns each OSC set into a WAYPOINT:
//     * a set arriving while block `a` is in flight is stamped at block
//       a + kLookahead (2) -- that block's START is where the value is exact;
//     * the FIRST set of a curve also anchors the OLD value at block `a`
//       (the plugin's current value), so activation is a 2-block approach
//       ramp, never a click;
//     * between waypoints the curve is a straight chord (piecewise-linear),
//       and inside each block the chord is rendered as a sub-block staircase
//       (segment = --substep samples; the chord value at the segment's first
//       sample is pushed to the plugin before that slice is processed).
//   This tool sends exactly two sets (Gain 0.0, then Gain 1.0) timed to land
//   at ~10% and ~90% of the window, captures the output sample-by-sample,
//   and dumps it so a plotter can overlay the EXACT expected staircase
//   (recomputed from the same algorithm + the detected waypoint stamps) on
//   the real audio. Residual should be ~float noise (1e-7), not algorithmic.
//
// SIGNAL: constant +1.0 in; smoke plugin Gain=1/PWM=1 -> out = Gain exactly.
// LINKS ONLY heddle_core (ring + OSC); headless.
//
// USAGE
//   heddle-osc-twopt <heddle-host> <smoke-vst3> [--block 512] [--substep 64]
//                    [--b0 8] [--b1 72] [--settle 2000] [--dump FILE.txt]
//
// The test waits until the host block estimate reaches (b0-2) before sending
// set 0, and (b1-2) before sending set 1 (arrival in block k stamps k+2).
// The actual stamps are detected from the output itself (descent onset /
// ascent completion) and reported in the dump header.
//
// Exit 0 = completed and dumped. Exit 1 = setup/IO error.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  #include <io.h>
#else
  #include <signal.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

using namespace heddle;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Child spawn/reap -- same idiom as osc_ramp_test.cpp.
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
    // The child's stderr is captured to a file so a host crash is diagnosable.
    static FILE* childErr = nullptr;
    if (!childErr)
        childErr = std::fopen("heddle_host_child_err.txt", "wb");
    if (childErr) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);  // keep on the console
        si.hStdError = (HANDLE)_get_osfhandle(_fileno(childErr));
    }
    PROCESS_INFORMATION pi{};
    ChildHandle h;
    if (CreateProcessA(exe.c_str(), cmdBuf.data(), nullptr, nullptr,
                       si.hStdError != nullptr, 0,
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
    DWORD code = 0;
    if (WaitForSingleObject(h.hProc, 30000) == WAIT_OBJECT_0) {
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
// Producer: constant +1.0, real-time paced (backpressure idiom from the
// ramp test).
struct Producer {
    AudioRing* ring = nullptr;
    int ch = 2;
    int block = 512;
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
                target += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::duration<double, std::ratio<1, 1000000000>>(
                        (double)n * 1e9 / (double)sr));
                while (!stop.load()) {
                    const auto now = Clock::now();
                    if (now >= target) break;
                    const auto rem =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(target - now).count();
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
                "usage: heddle-osc-twopt <heddle-host> <smoke-vst3> [--block 512] "
                "[--substep 64] [--whole]\n       [--b0 8] [--b1 72] [--settle 2000] "
                "[--dump FILE.txt]\n");
            return 2;
        }
        const std::string hostExe = argv[1];
        const std::string vst3 = argv[2];
        int block = 512, substep = 64, settleMs = 2000;
        bool whole = false;   // --whole: curve_resolution 0 (whole-queue raw path)
        bool usecase = false; // --usecase: the 4-op sub-block timing scenario
        int b0 = 8, b1 = 72;   // waypoint targets (10% / 90% of an 80-block window)
        std::string dumpPath;
        int sr = 48000;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };
            if (a == "--block") block = std::atoi(next());
            else if (a == "--substep") substep = std::atoi(next());
            else if (a == "--whole") whole = true;
            else if (a == "--usecase") usecase = true;
            else if (a == "--b0") b0 = std::atoi(next());
            else if (a == "--b1") b1 = std::atoi(next());
            else if (a == "--settle") settleMs = std::atoi(next());
            else if (a == "--rate") sr = std::atoi(next());
            else if (a == "--dump") dumpPath = next();
        }
        if (whole) substep = 0;                 // whole-queue raw path (0 = default)
        if (substep < 0) substep = 0;
        if (substep > 1 && block % substep != 0)
            std::fprintf(stderr, "WARN: substep %d does not divide block %d; the host "
                                 "clamps the last segment\n", block, substep);

    const int hostPort = 22100 + (block % 200);
    const std::string ringPrefix = "heddle.twopt";
    const std::string chainPath = dirOf(hostExe) + "heddle_osc_twopt_chain.json";
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

    AudioRing::remove(ringPrefix + ".in");
    AudioRing::remove(ringPrefix + ".out");
    std::vector<std::string> hargv = {hostExe, chainPath, "--audio", "ring",
                                      "--ring", ringPrefix,
                                      "--osc-port", std::to_string(hostPort),
                                      "--substep", std::to_string(substep)};
    ChildHandle child = spawnChild(hostExe, hargv);
    if (!child.valid) { std::fprintf(stderr, "FAIL: spawn heddle-host\n"); return 1; }
    const auto ringDeadline = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < ringDeadline &&
           !(AudioRing::exists(ringPrefix + ".in") && AudioRing::exists(ringPrefix + ".out"))) {
        if (childFinished(child)) { std::fprintf(stderr, "FAIL: host exited before rings\n"); childKill(child); return 1; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!(AudioRing::exists(ringPrefix + ".in") && AudioRing::exists(ringPrefix + ".out"))) {
        std::fprintf(stderr, "FAIL: rings did not appear\n");
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
    if (!client.openOut("127.0.0.1", hostPort, &err)) { std::fprintf(stderr, "FAIL: openOut: %s\n", err.c_str()); childKill(child); childReap(child); return 1; }
    client.send(OscMsg::make("/chain/query"));
    {
        OscMsg m;
        const auto dl = Clock::now() + std::chrono::seconds(5);
        bool got = false;
        while (Clock::now() < dl && !got) {
            if (client.poll(m) && m.addr == "/reply/chain" && !m.strs.empty()) got = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!got) { std::fprintf(stderr, "FAIL: no /reply/chain\n"); childKill(child); childReap(child); return 1; }
    }

    const int nCh = ringIn.numChannels();
    std::vector<float> scratch((size_t)block * nCh, 0.0f);
    Producer pr;
    pr.ring = &ringIn;
    pr.ch = nCh;
    pr.block = block;
    pr.sr = sr;
    pr.totalWritten.store(0);
    std::thread prod([&]() { pr.run(); });

    // Warm up to steady state; measure pipeline depth D0 (samples).
    long long outIndex = 0;
    float warmMax = 0.f;
    {
        const auto dl = Clock::now() + std::chrono::milliseconds(settleMs);
        while (Clock::now() < dl) {
            const size_t n = ringOut.read(scratch.data(), block);
            if (n > 0) {
                outIndex += (long long)n;
                for (size_t i = 0; i < n; ++i)
                    if (scratch[(size_t)i * nCh] > warmMax) warmMax = scratch[(size_t)i * nCh];
            } else std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    const long long D0 = pr.totalWritten.load(std::memory_order_relaxed) - outIndex;
    std::fprintf(stderr, "[twopt] warmup D0=%lld s (%.2f ms) warmMax=%.3f\n",
                 D0, (double)D0 / sr * 1000.0, warmMax);
    if (warmMax < 0.95f)
        std::fprintf(stderr, "WARN: pass-through not ~1.0; results suspect\n");

    // ---- the four-op USE-CASE run (sub-block timing) -------------------------
    // Sent as ONE burst while block bU is in flight (delay/duration in
    // BLOCKS, kLookahead-relative, first set is a no-op re-asserting 1.0):
    //   op1: set 1 (delay 0, dur -1)  -> no-op: already 1.0
    //   op2: set 0 (delay 0, dur 0)   -> HARD SNAP to 0 at block bU+2
    //   op3: set 1 (delay 0.5, dur 2.5) -> 2.5-block ramp 0->1 from bU+2.5
    //   op4: set 0 (delay 3, dur 0)   -> HARD SNAP to 0 at bU+5 (ramp end)
    if (usecase) {
        const long long bU = 24;
        const long long nBlocksTotal = bU + 9;   // capture ~2 blocks past last op
        std::vector<float> win;
        win.reserve((size_t)nBlocksTotal * block);
        bool burstSent = false;
        long long acks = 0;
        const auto hardDeadline = Clock::now() + std::chrono::seconds(60);
        while ((long long)win.size() < nBlocksTotal * (long long)block &&
               Clock::now() < hardDeadline) {
            const size_t n = ringOut.read(scratch.data(), block);
            if (n > 0) {
                for (size_t i = 0; i < n; ++i) win.push_back(scratch[(size_t)i * nCh]);
                outIndex += (long long)n;
            }
            OscMsg m;
            while (client.poll(m, nullptr))
                if (m.addr.rfind("/reply/", 0) == 0) ++acks;
            if (childFinished(child)) {
                std::fprintf(stderr, "FAIL: host died during capture (exit=%d, %lld samples)\n",
                             childReap(child), (long long)win.size());
                pr.stop.store(true); prod.join();
                return 1;
            }
            const long long est = ((long long)win.size() + D0) / block;
            if (!burstSent && est >= bU) {
                client.send(OscMsg::make("/chain/smoke/Gain", std::vector<float>{1.0f, 0.0f, -1.0f}));
                client.send(OscMsg::make("/chain/smoke/Gain", std::vector<float>{0.0f, 0.0f,  0.0f}));
                client.send(OscMsg::make("/chain/smoke/Gain", std::vector<float>{1.0f, 0.5f,  2.5f}));
                client.send(OscMsg::make("/chain/smoke/Gain", std::vector<float>{0.0f, 3.0f,  0.0f}));
                burstSent = true;
                std::fprintf(stderr, "[twopt] usecase burst sent @ est block %lld\n", est);
            }
            if (n == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        if (!burstSent) {
            std::fprintf(stderr, "FAIL: did not get to send the burst (est stopped early)\n");
            client.send(OscMsg::make("/chain/quit"));
            childKill(child); childReap(child); pr.stop.store(true); prod.join();
            return 1;
        }
        const std::string dump = dumpPath.empty() ? chainPath + ".dump" : dumpPath;
        {
            FILE* f = std::fopen(dump.c_str(), "wb");
            if (!f) std::fprintf(stderr, "FAIL: cannot write dump %s\n", dump.c_str());
            else {
                std::fprintf(f,
                    "# heddle-osc-twopt usecase=1 block=%d substep=0 sr=%d D0=%lld "
                    "bU=%lld acks=%lld n_samples=%lld "
                    "ops=[set1(0,-1) set0(0,0) set1(0.5,2.5) set0(3,0)]\n",
                    block, sr, D0, bU, acks, (long long)win.size());
                for (float v : win) std::fprintf(f, "%.9g\n", (double)v);
                std::fclose(f);
                std::fprintf(stderr, "[twopt] dumped %s (%lld samples)\n",
                             dump.c_str(), (long long)win.size());
            }
        }
        client.send(OscMsg::make("/chain/quit"));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        client.close();
        pr.stop.store(true);
        prod.join();
        ringIn.close();
        ringOut.close();
        childReap(child);
        AudioRing::remove(ringPrefix + ".in");
        AudioRing::remove(ringPrefix + ".out");
        std::remove(chainPath.c_str());
        std::printf("@@HEDDLE_OSC_TWOPT@@ usecase=1 block=%d D0=%lld bU=%lld acks=%lld dump=%s\n",
                    block, D0, bU, acks, dump.c_str());
        return 0;
    }

    // ---- the two-set run -----------------------------------------------------
    // Host-block estimate from the output cursor: host position ~ outIndex + D0.
    // Arrival in block k stamps the waypoint at k + 2 (kLookahead), so we send
    // set 0 at est >= b0-2 and set 1 at est >= b1-2.
    const long long nBlocksTotal = b1 + 14;   // capture well past the second set
    std::vector<float> win;
    win.reserve((size_t)nBlocksTotal * block);
    bool set0Sent = false, set1Sent = false;
    long long acks = 0;
    const auto hardDeadline = Clock::now() + std::chrono::seconds(60);
    while ((long long)win.size() < nBlocksTotal * (long long)block &&
           Clock::now() < hardDeadline) {
        const size_t n = ringOut.read(scratch.data(), block);
        if (n > 0) {
            for (size_t i = 0; i < n; ++i) win.push_back(scratch[(size_t)i * nCh]);
            outIndex += (long long)n;
        }
        OscMsg m;
        while (client.poll(m, nullptr)) {
            if (m.addr.rfind("/reply/", 0) == 0) ++acks;
        }
        if (childFinished(child)) {
            std::fprintf(stderr, "FAIL: host died during capture (exit=%d, %lld samples)\n",
                         childReap(child), (long long)win.size());
            pr.stop.store(true); prod.join();
            return 1;
        }
        const long long est = ((long long)win.size() + D0) / block;
        if (!set0Sent && est >= b0 - 2) {
            client.send(OscMsg::make("/chain/smoke/Gain", 0.0f));
            set0Sent = true;
            std::fprintf(stderr, "[twopt] set0 (Gain 0.0) sent @ est block %lld\n", est);
        } else if (!set1Sent && set0Sent && est >= b1 - 2) {
            client.send(OscMsg::make("/chain/smoke/Gain", 1.0f));
            set1Sent = true;
            std::fprintf(stderr, "[twopt] set1 (Gain 1.0) sent @ est block %lld\n", est);
        }
        if (n == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    if (!set0Sent || !set1Sent) {
        std::fprintf(stderr, "FAIL: did not get to send both sets (est stopped early)\n");
        client.send(OscMsg::make("/chain/quit"));
        childKill(child); childReap(child); pr.stop.store(true); prod.join();
        return 1;
    }

    // ---- detect the actual waypoint stamps from the output --------------------
    // Descent onset: first sample deviating from 1.0 after set 0. The first
    // segment of the anchor block still holds exactly 1.0 (chord exact at the
    // waypoint), so the onset sample lies INSIDE block a0 -> floor gives a0.
    long long iOn0 = -1;
    for (long long i = 0; i < (long long)win.size(); ++i)
        if (win[i] < 0.999f) { iOn0 = i; break; }
    const long long a0 = (iOn0 >= 0) ? (iOn0 + D0) / block : -1;
    // Ascent completion: last sample deviating from 1.0 (before the tail hold).
    long long iOff1 = -1;
    for (long long i = (long long)win.size() - 1; i >= 0; --i)
        if (1.0f - win[i] > 1e-4f) { iOff1 = i; break; }
    const long long b1act = (iOff1 >= 0) ? (iOff1 + D0) / block + 1 : -1;

    std::fprintf(stderr, "[twopt] D0=%lld  a0(anchor set0)=%lld  wp0=%lld  b1(wp1)=%lld  acks=%lld\n",
                 D0, a0, a0 + 2, b1act, acks);

    // ---- dump -----------------------------------------------------------------
    const std::string dump = dumpPath.empty() ? chainPath + ".dump" : dumpPath;
    {
        FILE* f = std::fopen(dump.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "FAIL: cannot write dump %s\n", dump.c_str()); }
        else {
            std::fprintf(f,
                "# heddle-osc-twopt block=%d substep=%d sr=%d D0=%lld "
                "a0=%lld wp0=%lld b1=%lld b0_target=%d b1_target=%d acks=%lld "
                "n_samples=%lld\n",
                block, substep, sr, D0, a0, a0 + 2, b1act, b0, b1, acks,
                (long long)win.size());
            for (float v : win) std::fprintf(f, "%.9g\n", (double)v);
            std::fclose(f);
            std::fprintf(stderr, "[twopt] dumped %s (%lld samples)\n",
                         dump.c_str(), (long long)win.size());
        }
    }

    client.send(OscMsg::make("/chain/quit"));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    client.close();
    pr.stop.store(true);
    prod.join();
    ringIn.close();
    ringOut.close();
    childReap(child);
    AudioRing::remove(ringPrefix + ".in");
    AudioRing::remove(ringPrefix + ".out");
    std::remove(chainPath.c_str());

    std::printf("@@HEDDLE_OSC_TWOPT@@ block=%d substep=%d D0=%lld a0=%lld wp0=%lld b1=%lld acks=%lld dump=%s\n",
                block, substep, D0, a0, a0 + 2, b1act, acks, dump.c_str());
    return 0;
}
