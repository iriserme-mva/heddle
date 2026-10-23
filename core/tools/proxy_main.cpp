// heddle-proxy (standalone) — OSC fan-out router as a CLI tool.
//
// Routes one control surface (TouchOSC etc.) to up to 5 Heddle instances
// over OSC/UDP. Same engine as the passthrough VST3 (heddle_core's
// ProxyEngine); this executable adds a config-file/CLI front end, a
// discovery helper (--query), and a live stats line. See docs/PROXY.md.
//
// Usage:
//   heddle-proxy [config.json] [options]
//     --listen <port>                 override listen port
//     --mode off|only-control|full    override mode
//     --target <ip:port>              add a target (repeatable, max 5)
//     --route <src> <tgt> <slot> <param>   add a route (repeatable)
//     --query <ip:port>               discover a target's slots/params, print, exit
//     --timeout <ms>                  discovery timeout (default 1500)
//     --stats <sec>                   stats line period on stderr (0 = off)
//     -h, --help                      this text
//
// With no config file and no options it errors out (a config is required).
// Exit codes: 0 = ran until killed / --query / --help ok; 1 = bad config or
// usage; 2 = socket error.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "heddle/proxy.hpp"
#include "heddle/proxy_engine.hpp"

using namespace heddle;

namespace {

void printUsage(FILE* out) {
    std::fprintf(out,
        "heddle-proxy — route one OSC surface to up to %d Heddle targets\n"
        "\n"
        "usage: heddle-proxy [config.json] [options]\n"
        "  --listen <port>                   override listen port\n"
        "  --mode off|only-control|full      override routing mode\n"
        "  --target <ip:port>                add a target (repeatable, max %d)\n"
        "  --route <src> <tgt> <slot> <param>   add a route (repeatable)\n"
        "      src = surface address, tgt = target index, slot/param = Heddle\n"
        "  --query <ip:port>                 discover target params, print, exit\n"
        "  --timeout <ms>                    discovery timeout (default 1500)\n"
        "  --stats <sec>                     stats line period on stderr (0=off)\n"
        "  -h, --help                        this text\n"
        "exit: 0 ok; 1 bad config/usage; 2 socket error\n",
        ProxyConfig::kMaxTargets, ProxyConfig::kMaxTargets);
}

// Split "ip:port" (port is the last ':'-separated field).
bool parseIpPort(const std::string& s, std::string* ip, int* port, std::string* err) {
    const size_t p = s.rfind(':');
    if (p == std::string::npos || p == 0 || p + 1 >= s.size()) {
        if (err) *err = "expected ip:port, got '" + s + "'";
        return false;
    }
    *ip = s.substr(0, p);
    bool digits = !s.substr(p + 1).empty();
    for (char c : s.substr(p + 1))
        if (c < '0' || c > '9') { digits = false; break; }
    if (!digits) { if (err) *err = "bad port in '" + s + "'"; return false; }
    *port = 0;
    for (char c : s.substr(p + 1)) *port = *port * 10 + (c - '0');
    if (*port <= 0 || *port > 65535) { if (err) *err = "port out of range in '" + s + "'"; return false; }
    return true;
}

std::atomic<bool> g_stop{false};

void onSignal(int) {
    g_stop = true;
}

bool applyOverrides(ProxyConfig& c, int argc, char** argv, std::string* err) {
    auto need = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) { *err = std::string("missing value for ") + flag; return nullptr; }
        return argv[++i];
    };
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--listen") {
            const char* v = need(i, "--listen"); if (!v) return false;
            const std::string vs = v;
            bool digits = false;
            for (char c : vs) { if (c < '0' || c > '9') break; digits = true; }
            if (!digits) { *err = "bad --listen port"; return false; }
            int port = 0;
            for (char c : vs) port = port * 10 + (c - '0');
            c.listenPort = port;
        } else if (a == "--mode") {
            const char* v = need(i, "--mode"); if (!v) return false;
            c.mode = v;
        } else if (a == "--target") {
            const char* v = need(i, "--target"); if (!v) return false;
            if ((int) c.targets.size() >= ProxyConfig::kMaxTargets) {
                *err = "too many --target (max " +
                       std::to_string(ProxyConfig::kMaxTargets) + ")";
                return false;
            }
            ProxyTarget t;
            if (!parseIpPort(v, &t.ip, &t.port, err)) return false;
            c.targets.push_back(std::move(t));
        } else if (a == "--route") {
            const char* src = need(i, "--route src"); if (!src) return false;
            const char* tgtv = need(i, "--route tgt"); if (!tgtv) return false;
            const char* slot = need(i, "--route slot"); if (!slot) return false;
            const char* param = need(i, "--route param"); if (!param) return false;
            const std::string tgt = tgtv;
            bool digits = !tgt.empty();
            for (char c : tgt) if (c < '0' || c > '9') { digits = false; break; }
            if (!digits) { *err = "--route target index must be an integer"; return false; }
            int ti = 0;
            for (char c : tgt) ti = ti * 10 + (c - '0');
            c.routes.push_back({src, ti, slot, param});
        }
        // --query/--timeout/--stats/-h are handled by the caller.
    }
    return true;
}

int runQuery(const std::string& ipPort, int timeoutMs) {
    ProxyTarget t;
    std::string err;
    if (!parseIpPort(ipPort, &t.ip, &t.port, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    ProxyEngine engine;
    ProxyDiscovered d = engine.discover(t, timeoutMs);
    if (!d.ok) {
        std::fprintf(stderr, "no reply from %s (timeout %d ms)\n", ipPort.c_str(), timeoutMs);
        return 1;
    }
    std::printf("%s: %zu params\n", ipPort.c_str(), d.params.size());
    for (const auto& p : d.params)
        std::printf("  /chain/%s/%s   (param id %d)\n", p.slot.c_str(), p.param.c_str(), p.paramId);
    if (d.params.empty()) std::printf("  (no params)\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string configFile;
    int timeoutMs = 1500;
    double statsSec = 2.0;
    bool queryMode = false;
    std::string queryTarget;

    // First pass: locate the config file + the flags applyOverrides() does
    // not own (--query/--timeout/--stats/-h). Flag VALUES must be skipped —
    // otherwise "--listen 43100" would treat "43100" as a config file.
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { printUsage(stdout); return 0; }
        if (a == "--query") {
            queryMode = true;
            if (i + 1 < argc) queryTarget = argv[++i];
            continue;
        }
        if (a == "--timeout") { if (i + 1 < argc) timeoutMs = std::atoi(argv[++i]); continue; }
        if (a == "--stats") { if (i + 1 < argc) statsSec = std::atof(argv[++i]); continue; }
        // Values consumed by applyOverrides() — skip them here.
        if (a == "--listen" || a == "--mode" || a == "--target") {
            if (i + 1 < argc) ++i;
            continue;
        }
        if (a == "--route") {
            if (i + 4 < argc) i += 4;
            continue;
        }
        if (!a.empty() && a[0] != '-') {
            if (!configFile.empty()) {
                std::fprintf(stderr, "error: only one config file allowed\n");
                return 1;
            }
            configFile = a;
            continue;
        }
    }

    if (queryMode)
        return runQuery(queryTarget, timeoutMs);

    // Config: file (if given) or pure-CLI overrides.
    ProxyConfig cfg;
    std::string err;
    if (!configFile.empty()) {
        if (!proxy::parseFile(configFile, cfg, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
    }
    int rc = applyOverrides(cfg, argc - 1, argv + 1, &err) ? 0 : 1;
    if (rc != 0) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (cfg.listenPort <= 0) {
        std::fprintf(stderr, "error: no listen port (config or --listen)\n");
        return 1;
    }

    ProxyEngine engine;
    if (!engine.start(cfg, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }
    std::fprintf(stderr,
        "heddle-proxy: listening on %s:%d  mode=%s  targets=%zu  routes=%zu\n",
        cfg.listenHost.c_str(), cfg.listenPort, cfg.mode.c_str(),
        cfg.targets.size(), cfg.routes.size());

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifdef _WIN32
    // Windows console Ctrl-C also routes through the SIGINT handler.
#endif

    const auto statsEvery = std::chrono::milliseconds((int) (statsSec * 1000.0));
    auto lastStats = std::chrono::steady_clock::now();
    long long lastR = 0, lastF = 0, lastD = 0;
    while (!g_stop) {
        engine.pump();
        const auto now = std::chrono::steady_clock::now();
        if (statsEvery.count() > 0 && now - lastStats >= statsEvery) {
            lastStats = now;
            const long long r = engine.received(), f = engine.forwarded(), d = engine.dropped();
            if (r != lastR || f != lastF || d != lastD) {
                std::fprintf(stderr,
                    "  [+%lld rec / +%lld fwd / +%lld drop]  total rec=%lld fwd=%lld drop=%lld\n",
                    r - lastR, f - lastF, d - lastD, r, f, d);
            }
            lastR = r; lastF = f; lastD = d;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::fprintf(stderr, "heddle-proxy: stopping (rec=%lld fwd=%lld drop=%lld)\n",
                 engine.received(), engine.forwarded(), engine.dropped());
    engine.stop();
    return 0;
}
