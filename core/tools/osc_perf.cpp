// heddle_osc_perf -- OSC performance benchmark for the CORE layer (no JUCE).
//
// Measures the pieces that determine how smoothly a control client (Ableton
// OSC, a Max patch, a Python script, ...) can drive a live Heddle host:
//
//   * codec    : oscEncode/oscDecode throughput (Mparam-msg/s, MB/s wire)
//   * param    : the continuous param-adjust path a /chain/<slot>/<param> set
//                exercises in the core: classify() + resolveSlot() +
//                setParam(). Reported as Msets/s and per-op cost.
//   * jitter   : the cost distribution of those param sets (mean/median/
//                p99/max + p99/mean ratio) -- "smoothness" of continuous
//                adjustment at the core. A big p99/mean means occasional
//                stalls a control client would feel.
//   * loopback : real UDP over 127.0.0.1 -- one-way delivery latency (send ->
//                next poll returns) and full set->ack round-trip latency
//                (send set, classify, reply, client receives ack), with
//                mean/median/p99/max. This is the "realistic latency" of one
//                OSC hop, independent of any audio thread.
//   * maxrate  : the highest sustained set rate the core path can hold within
//                a 1 ms per-op budget (a comfortable sub-block budget).
//
// It is deliberately independent of the JUCE host, the audio device, and any
// real-time pacing, so it runs on every CI runner (Ubuntu/Windows/macOS) in
// the `core` job with no audio device or X server. The host-level numbers
// (set->ack RTT measured against a REAL heddle-host with a live audio thread,
// plus the audio-dropout rate under set load) come from
// `heddle_osc_perf_host` in the host job -- see docs/PERFORMANCE.md.
//
// Usage:
//   heddle_osc_perf [--iters N] [--loopback N] [--params N] [--verbose]
//
// Prints a human table plus a machine-readable block delimited by
//   @@HEDDLE_OSC_PERF@@ ... @@END@@
// (KEY<TAB>value lines) so the CI step can archive the numbers and so
// docs/PERFORMANCE.md can cite a single reproducible run.
//
// Exit 0 = all measurements completed. A SLOW result is not a failure --
// these are measurements, not pass/fail thresholds.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "heddle/chain.hpp"
#include "heddle/osc.hpp"
#include "heddle/osc_io.hpp"
#include "heddle/param.hpp"

using namespace heddle;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Minimal mock backend so the param path (classify -> resolve -> set) can be
// exercised without a plugin. Mirrors test_chain.cpp's MockBackend, reduced
// to what a single-slot set needs.
class MockBackend : public IPluginBackend {
public:
    explicit MockBackend(int nParams) {
        for (int i = 0; i < nParams; ++i) {
            Param p;
            p.id = static_cast<uint32_t>(i);
            p.title = "P" + std::to_string(i);
            p.defaultValue = 0.5f;
            p.value = 0.5f;
            p.minValue = 0.f;
            p.maxValue = 1.f;
            p.kind = ParamKind::Float;
            params_.push_back(p);
        }
    }
    bool load(const SlotConfig& slot, ParamSet& out) override {
        (void)slot;
        out.pluginId = "bench";
        out.params = params_;
        return true;
    }
    bool setParams(const std::string&,
                   const std::vector<std::pair<std::string, float>>&) override {
        return true;
    }
    bool setParam(const std::string&, uint32_t, float) override { return true; }

private:
    std::vector<Param> params_;
};

// ---------------------------------------------------------------------------
// Statistics over a vector of per-op durations (ns).
struct Stats {
    double n = 0;
    double mean = 0;
    double median = 0;
    double p99 = 0;
    double min = 0;
    double max = 0;
    double stddev() const {
        if (n < 2) return 0;
        const double v = sumSq_ / n - mean * mean;
        return std::sqrt(v > 0 ? v : 0);
    }
    static Stats fromNanos(std::vector<double> ns) {
        Stats s;
        if (ns.empty()) return s;
        std::sort(ns.begin(), ns.end());
        const size_t m = ns.size();
        s.n = static_cast<double>(m);
        s.min = ns.front();
        s.max = ns.back();
        double sum = 0;
        for (double v : ns) {
            sum += v;
            s.sumSq_ += v * v;
        }
        s.mean = sum / m;
        s.median = ns[m / 2];
        s.p99 = ns[static_cast<size_t>(std::min(m - 1, (m * 99) / 100))];
        return s;
    }

private:
    double sumSq_ = 0;
};

static double ns2us(double ns) { return ns / 1000.0; }

// ---------------------------------------------------------------------------
// [codec] Encode a full "query-all reply" (nParams one-line string messages)
// and decode every message back. Repeated over the wall clock so the
// throughput figure is stable and allocation is amortized.
struct CodecResult {
    Stats perBatch;   // duration of one full encode+decode batch
    double mbPerSec = 0;
    double mParamMsgsPerSec = 0;
    int nParams = 0;
};

static CodecResult benchCodec(int iters, int nParams) {
    // One representative reply line per param (the largest per-message shape
    // the core emits): "id<TAB>title<TAB>value<TAB>min<TAB>max<TAB>steps<TAB>kind".
    std::vector<std::string> lines;
    lines.reserve(nParams);
    for (int i = 0; i < nParams; ++i)
        lines.push_back(std::to_string(i) + "\tP" + std::to_string(i) +
                        "\t0.5\t0\t1\t0\tfloat");

    // Warm up (allocator growth, branch prediction) outside the timing.
    {
        std::string scratch;
        scratch.reserve(1 << 16);
        for (const auto& ln : lines)
            scratch += oscEncode(OscMsg::make("/reply/bench/P0", ln));
        OscMsg d;
        for (const auto& ln : lines) {
            const auto e = oscEncode(OscMsg::make("/reply/bench/P0", ln));
            oscDecode(e.data(), e.size(), d);
        }
    }

    std::vector<double> perBatchNs;
    perBatchNs.reserve(iters);
    long long totalBytes = 0;
    for (int i = 0; i < iters; ++i) {
        const auto t0 = Clock::now();
        for (int p = 0; p < nParams; ++p) {
            const auto enc = oscEncode(OscMsg::make("/reply/bench/P" + std::to_string(p), lines[p]));
            totalBytes += static_cast<long long>(enc.size());
            OscMsg d;
            if (!oscDecode(enc.data(), enc.size(), d)) {
                std::fprintf(stderr, "codec: unexpected decode failure\n");
                return {};
            }
        }
        const auto t1 = Clock::now();
        perBatchNs.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() * 1.0);
    }
    CodecResult r;
    r.nParams = nParams;
    r.perBatch = Stats::fromNanos(perBatchNs);
    const double totalNs =
        std::accumulate(perBatchNs.begin(), perBatchNs.end(), 0.0);
    const double totalSec = totalNs / 1e9;
    if (totalSec > 0) {
        r.mbPerSec = (static_cast<double>(totalBytes) / totalSec) / 1e6;
        r.mParamMsgsPerSec = (static_cast<double>(nParams) * iters / totalSec) / 1e6;
    }
    return r;
}

// ---------------------------------------------------------------------------
// [param] The exact per-OSC-set core path: classify() -> resolveSlot() ->
// setParam(), varying the param name so findByName() does the real lookup.
static Stats benchParamPath(Chain& chain, const std::string& slot, int nParams, int iters) {
    // Warm up.
    for (int i = 0; i < 1000; ++i) {
        OscMsg m;
        m.addr = "/chain/" + slot + "/P" + std::to_string(i % nParams);
        m.floats.push_back(0.5f);
        m.typeTags.push_back('f');
        const auto a = osc_addr::classify(m);
        if (chain.resolveSlot(a.slotToken) >= 0)
            chain.setParam(a.slotToken, a.param, a.value);
    }

    std::vector<double> ns;
    ns.reserve(iters);
    for (int i = 0; i < iters; ++i) {
        const std::string name = "P" + std::to_string(i % nParams);
        OscMsg msg;
        msg.addr = "/chain/" + slot + "/" + name;
        msg.floats.push_back(0.5f);
        msg.typeTags.push_back('f');
        const auto t0 = Clock::now();
        const auto a = osc_addr::classify(msg);
        const int idx = chain.resolveSlot(a.slotToken);
        if (idx >= 0)
            chain.setParam(a.slotToken, a.param, a.value);
        const auto t1 = Clock::now();
        ns.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() * 1.0);
    }
    return Stats::fromNanos(ns);
}

// ---------------------------------------------------------------------------
// [loopback] One-way UDP delivery latency over 127.0.0.1:
// send -> the next server.poll() returns the datagram. Non-blocking poll,
// spin (no sleep) so we measure delivery, not poll cadence.
static Stats benchLoopbackOneWay(int iters, std::string* err) {
    OscUdp server, client;
    if (!server.bind("127.0.0.1", 0, err)) return {};
    if (!client.bind("127.0.0.1", 0, err)) return {};
    if (!client.openOut("127.0.0.1", server.localPort(), err)) return {};

    for (int i = 0; i < 128; ++i) {  // warm up
        client.send(OscMsg::make("/warm", static_cast<float>(i)));
        OscMsg m;
        while (!server.poll(m, nullptr)) {
        }
    }

    std::vector<double> ns;
    ns.reserve(iters);
    for (int i = 0; i < iters; ++i) {
        const auto t0 = Clock::now();
        client.send(OscMsg::make("/ping", static_cast<float>(i)));
        OscMsg m;
        while (!server.poll(m, nullptr)) {
        }
        const auto t1 = Clock::now();
        ns.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() * 1.0);
    }
    client.close();
    server.close();
    return Stats::fromNanos(ns);
}

// ---------------------------------------------------------------------------
// [loopback] Full set -> ack round trip over 127.0.0.1: client sends a
// /chain/<slot>/<param> set; the server classifies it (real core cost) and
// replies to the captured source; the client waits for the ack. This is the
// realistic latency of ONE OSC control hop through the core path.
static Stats benchLoopbackRTT(int iters, std::string* err) {
    OscUdp server, client;
    if (!server.bind("127.0.0.1", 0, err)) return {};
    if (!client.bind("127.0.0.1", 0, err)) return {};
    if (!client.openOut("127.0.0.1", server.localPort(), err)) return {};

    for (int i = 0; i < 128; ++i) {  // warm up, then drain
        client.send(OscMsg::make("/chain/bench/P0", 0.5f));
        OscMsg m;
        OscUdp::SourceAddr src;
        while (!server.poll(m, &src)) {
        }
        server.sendTo(OscMsg::make("/reply/bench/P0", 0.5f), src);
        while (!client.poll(m, nullptr)) {
        }
    }
    {  // drain any stragglers
        OscMsg m;
        while (client.poll(m, nullptr)) {
        }
        while (server.poll(m, nullptr)) {
        }
    }

    std::vector<double> ns;
    ns.reserve(iters);
    for (int i = 0; i < iters; ++i) {
        const auto t0 = Clock::now();
        client.send(OscMsg::make("/chain/bench/P0", 0.5f));
        OscMsg m;
        OscUdp::SourceAddr src;
        while (!server.poll(m, &src)) {
        }
        osc_addr::classify(m);  // real per-set interpretation cost
        server.sendTo(OscMsg::make("/reply/bench/P0", 0.5f), src);
        OscMsg a;
        while (!client.poll(a, nullptr)) {
        }
        const auto t1 = Clock::now();
        ns.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() * 1.0);
    }
    client.close();
    server.close();
    return Stats::fromNanos(ns);
}

// ---------------------------------------------------------------------------
static void printStats(const char* name, const Stats& s) {
    std::printf("  %-9s n=%-6.0f mean=%10.2f us   med=%9.2f   p99=%10.2f   max=%12.2f   stddev=%9.2f us\n",
                name, s.n, ns2us(s.mean), ns2us(s.median), ns2us(s.p99),
                ns2us(s.max), ns2us(s.stddev()));
}

static void emit(const std::string& key, double v, int prec = 3) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
    std::printf("%s\t%s\n", key.c_str(), buf);
}

int main(int argc, char** argv) {
    int iters = 200000;      // param-path iterations
    int loopback = 40000;    // loopback iterations
    int codecIters = 20000;  // codec batches (each = nParams messages)
    int nParams = 64;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            return (i + 1 < argc) ? argv[++i] : "0";
        };
        if (a == "--iters") iters = std::atoi(next());
        else if (a == "--loopback") loopback = std::atoi(next());
        else if (a == "--codec-iters") codecIters = std::atoi(next());
        else if (a == "--params") nParams = std::atoi(next());
        else if (a == "--verbose") verbose = true;
        else if (a == "--help" || a == "-h") {
            std::printf("usage: %s [--iters N] [--loopback N] [--codec-iters N] [--params N]\n",
                        argv[0]);
            return 0;
        }
    }
    if (iters < 1000) iters = 1000;
    if (loopback < 1000) loopback = 1000;
    if (codecIters < 1000) codecIters = 1000;
    if (nParams < 1) nParams = 1;

    std::printf("heddle_osc_perf  (core-layer OSC benchmark; nParams=%d, iters=%d, loopback=%d)\n",
                nParams, iters, loopback);
    std::printf("  codec    : OSC wire encode+decode throughput\n");
    std::printf("  param    : classify->resolveSlot->setParam per-op cost (continuous adjust)\n");
    std::printf("  loopback : UDP 127.0.0.1 one-way delivery + set->ack RTT\n");
    std::printf("\n[codec] encode+decode of a %d-param reply (per param message):\n", nParams);

    const CodecResult codec = benchCodec(codecIters, nParams);
    if (codec.perBatch.n <= 0) {
        std::fprintf(stderr, "codec: benchmark produced no data\n");
        return 1;
    }
    printStats("codec", codec.perBatch);
    std::printf("  codec throughput: %.1f Mparam-msg/s , %.2f MB/s wire\n",
                codec.mParamMsgsPerSec, codec.mbPerSec);

    std::printf("\n[param] per-op cost of one OSC set through the core path "
                "(classify + resolveSlot + setParam):\n");
    MockBackend be(nParams);
    ChainConfig cfg;
    cfg.sampleRate = 48000;
    cfg.blockSize = 1024;
    SlotConfig sc;
    sc.id = "bench";
    sc.pluginPath = "bench://mock";
    cfg.slots = {sc};
    Chain chain(be);
    std::string cerr;
    if (!chain.load(cfg, &cerr)) {
        std::fprintf(stderr, "param: chain load failed: %s\n", cerr.c_str());
        return 1;
    }
    const Stats param = benchParamPath(chain, "bench", nParams, iters);
    printStats("param", param);
    const double paramSetsPerSec = param.mean > 0 ? (1e9 / param.mean) : 0;
    const double paramJitter = param.mean > 0 ? (param.p99 / param.mean) : 0;
    // Max sustainable set rate within a 1 ms per-op budget (comfortable
    // sub-block budget; a single set must never dominate an audio block).
    const double maxSetRate1ms = param.mean > 0 ? (1e6 / param.mean) : 0;
    std::printf("  param throughput: %.1f Msets/s , p99/mean jitter x%.2f , "
                "max rate @1ms/op budget: %.2f ksets/s\n",
                paramSetsPerSec / 1e6, paramJitter, maxSetRate1ms / 1e3);

    std::printf("\n[loopback] UDP 127.0.0.1 one-way delivery + set->ack RTT:\n");
    std::string err;
    const Stats oneway = benchLoopbackOneWay(loopback, &err);
    if (oneway.n > 0)
        printStats("oneway", oneway);
    else
        std::printf("  oneway    : skipped (%s)\n", err.c_str());
    const Stats rtt = benchLoopbackRTT(loopback, &err);
    if (rtt.n > 0)
        printStats("rtt", rtt);
    else
        std::printf("  rtt       : skipped (%s)\n", err.c_str());

    std::printf("\n@@HEDDLE_OSC_PERF@@\n");
    std::printf("nParams\t%d\n", nParams);
    std::printf("iters\t%d\n", iters);
    std::printf("loopback\t%d\n", loopback);
    emit("codec.mean.us", ns2us(codec.perBatch.mean));
    emit("codec.p99.us", ns2us(codec.perBatch.p99));
    emit("codec.max.us", ns2us(codec.perBatch.max));
    emit("codec.mparammsg_per_s", codec.mParamMsgsPerSec, 1);
    emit("codec.mb_per_s", codec.mbPerSec, 1);
    emit("param.mean.us", ns2us(param.mean));
    emit("param.median.us", ns2us(param.median));
    emit("param.p99.us", ns2us(param.p99));
    emit("param.max.us", ns2us(param.max));
    emit("param.stddev.us", ns2us(param.stddev()));
    emit("param.msets_per_s", paramSetsPerSec / 1e6, 2);
    emit("param.jitter.p99_over_mean", paramJitter, 2);
    emit("param.max_set_rate_1ms_budget.ksets_per_s", maxSetRate1ms / 1e3, 2);
    if (oneway.n > 0) {
        emit("loopback.oneway.mean.us", ns2us(oneway.mean));
        emit("loopback.oneway.median.us", ns2us(oneway.median));
        emit("loopback.oneway.p99.us", ns2us(oneway.p99));
        emit("loopback.oneway.max.us", ns2us(oneway.max));
    }
    if (rtt.n > 0) {
        emit("loopback.rtt.mean.us", ns2us(rtt.mean));
        emit("loopback.rtt.median.us", ns2us(rtt.median));
        emit("loopback.rtt.p99.us", ns2us(rtt.p99));
        emit("loopback.rtt.max.us", ns2us(rtt.max));
    }
    std::printf("@@END@@\n");
    (void)verbose;
    return 0;
}
