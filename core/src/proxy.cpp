// heddle-proxy engine: OSC fan-out router + config (see docs/PROXY.md).
//
// Links only heddle_core (OSC codec + UDP transport + nlohmann json). No
// JUCE: the passthrough VST3 and the standalone exe both include this, and
// only the VST3 pulls JUCE (in its own TU) for audio + editor.
#include "heddle/proxy_engine.hpp"
#include "heddle/proxy.hpp"

#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <thread>

#include "nlohmann/json.hpp"

namespace heddle {

using json = nlohmann::json;

// ===========================================================================
// ProxyConfig validation
// ===========================================================================
bool ProxyConfig::validate(const ProxyConfig& c, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    if (c.listenPort <= 0 || c.listenPort > 65535)
        return fail("listen_port must be 1..65535");
    if (c.listenHost.empty())
        return fail("listen_host must not be empty");
    if (c.mode != "off" && c.mode != "only-control" && c.mode != "full")
        return fail("mode must be \"off\", \"only-control\", or \"full\"");
    if ((int) c.targets.size() > kMaxTargets)
        return fail("at most " + std::to_string(kMaxTargets) + " targets");
    for (size_t i = 0; i < c.targets.size(); ++i) {
        const auto& t = c.targets[i];
        if (t.ip.empty())
            return fail("target " + std::to_string(i) + ": ip must not be empty");
        if (t.port <= 0 || t.port > 65535)
            return fail("target " + std::to_string(i) + ": port must be 1..65535");
    }
    for (size_t i = 0; i < c.routes.size(); ++i) {
        const auto& r = c.routes[i];
        if (r.srcAddr.empty() || r.srcAddr[0] != '/')
            return fail("route " + std::to_string(i) +
                        ": src address must start with '/'");
        if (r.targetIdx < 0 || r.targetIdx >= (int) c.targets.size())
            return fail("route " + std::to_string(i) +
                        ": target index out of range");
        if (r.slot.empty() || r.param.empty())
            return fail("route " + std::to_string(i) +
                        ": slot and param must be non-empty");
    }
    return true;
}

// ===========================================================================
// JSON parse / dump
// ===========================================================================
namespace {
ProxyTarget targetFromJson(const json& o, std::string& err) {
    ProxyTarget t;
    if (o.contains("ip") && o["ip"].is_string())
        t.ip = o["ip"].get<std::string>();
    if (o.contains("port") && o["port"].is_number_integer())
        t.port = o["port"].get<int>();
    if (o.contains("enabled") && o["enabled"].is_boolean())
        t.enabled = o["enabled"].get<bool>();
    (void) err;
    return t;
}
ProxyRoute routeFromJson(const json& o, std::string& err) {
    ProxyRoute r;
    if (o.contains("src") && o["src"].is_string())
        r.srcAddr = o["src"].get<std::string>();
    if (o.contains("target") && o["target"].is_number_integer())
        r.targetIdx = o["target"].get<int>();
    if (o.contains("slot") && o["slot"].is_string())
        r.slot = o["slot"].get<std::string>();
    if (o.contains("param") && o["param"].is_string())
        r.param = o["param"].get<std::string>();
    (void) err;
    return r;
}
}  // namespace

namespace proxy {

bool parse(const std::string& jsonText, ProxyConfig& out, std::string* err) {
    json j;
    try {
        j = json::parse(jsonText);
    } catch (const json::exception& e) {
        if (err) *err = std::string("invalid JSON: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        if (err) *err = "config root must be an object";
        return false;
    }
    ProxyConfig c;
    if (j.contains("listen_port") && j["listen_port"].is_number_integer())
        c.listenPort = j["listen_port"].get<int>();
    if (j.contains("listen_host") && j["listen_host"].is_string())
        c.listenHost = j["listen_host"].get<std::string>();
    if (j.contains("mode") && j["mode"].is_string())
        c.mode = j["mode"].get<std::string>();
    if (j.contains("targets") && j["targets"].is_array()) {
        for (const auto& t : j["targets"])
            if (t.is_object())
                c.targets.push_back(targetFromJson(t, *err));
    }
    if (j.contains("routes") && j["routes"].is_array()) {
        for (const auto& r : j["routes"])
            if (r.is_object())
                c.routes.push_back(routeFromJson(r, *err));
    }
    if (!ProxyConfig::validate(c, err))
        return false;
    out = std::move(c);
    return true;
}

bool parseFile(const std::string& path, ProxyConfig& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open proxy config: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse(ss.str(), out, err);
}

std::string dump(const ProxyConfig& c) {
    json j;
    j["listen_port"] = c.listenPort;
    j["listen_host"] = c.listenHost;
    j["mode"] = c.mode;
    json targets = json::array();
    for (const auto& t : c.targets)
        targets.push_back(json{{"ip", t.ip}, {"port", t.port},
                               {"enabled", t.enabled}});
    j["targets"] = std::move(targets);
    json routes = json::array();
    for (const auto& r : c.routes)
        routes.push_back(json{{"src", r.srcAddr}, {"target", r.targetIdx},
                              {"slot", r.slot}, {"param", r.param}});
    j["routes"] = std::move(routes);
    return j.dump(2);
}

}  // namespace proxy

// ===========================================================================
// ProxyEngine
// ===========================================================================
struct ProxyEngine::Impl {
    OscUdp listen;                          // bound socket (surface -> us)
    std::vector<OscUdp> out;                // one per cfg_.targets (send only)
    std::vector<OscUdp::SourceAddr> dests;  // fixed dest per target
};

ProxyEngine::ProxyEngine() : impl_(new Impl()) {}
ProxyEngine::~ProxyEngine() { stop(); delete impl_; }

bool ProxyEngine::start(const ProxyConfig& cfg, std::string* err) {
    stop();  // clean any prior state
    std::string v;
    if (!ProxyConfig::validate(cfg, &v)) {
        if (err) *err = v;
        return false;
    }
    cfg_ = cfg;

    // Rebuild the routing map for the editor's "N sources" display. The
    // ACTUAL forwarding in pump() iterates cfg_.routes directly and matches
    // m.addr against each route.srcAddr — so one source can map to several
    // targets, and one (source,target) can map to several slot/params, each
    // forwarded as its own rewritten message. routeKeys_ only collapses to a
    // distinct-source count.
    routeKeys_.clear();
    std::map<std::string, size_t> keyOf;
    for (const auto& r : cfg_.routes) {
        auto it = keyOf.find(r.srcAddr);
        if (it == keyOf.end()) {
            keyOf[r.srcAddr] = routeKeys_.size();
            routeKeys_.push_back({r.srcAddr, {}});
        }
        if (!r.slot.empty()) routeKeys_[keyOf[r.srcAddr]].targetIdx.push_back(r.targetIdx);
    }

    // Bind the listen socket (mode "off" still binds so the surface sees us
    // alive and the counters reflect traffic we chose to drop — matching the
    // "3-state" semantics: off = audibly passthrough, but the proxy still
    // listens and can be switched live without the surface re-connecting).
    std::string berr;
    if (!impl_->listen.bind(cfg_.listenHost, cfg_.listenPort, &berr)) {
        if (err) *err = "listen bind failed (" + berr + "): " +
                        cfg_.listenHost + ":" + std::to_string(cfg_.listenPort);
        return false;
    }
    // One output socket per target (enabled or not; disabled are simply never
    // sent to, so a target can be re-enabled live without re-binding).
    impl_->out.clear();
    impl_->dests.clear();
    impl_->out.reserve(cfg_.targets.size());
    impl_->dests.reserve(cfg_.targets.size());
    for (const auto& t : cfg_.targets) {
        impl_->out.emplace_back();
        std::string oerr;
        if (!t.ip.empty() && t.port > 0) {
            // OscUdp::openOut requires a prior bind (see test_osc_udp); bind
            // an ephemeral local port first.
            if (!impl_->out.back().bind("0.0.0.0", 0, &oerr)) {
                if (err) *err = "out bind failed for " + t.ip + ":" +
                                std::to_string(t.port) + " (" + oerr + ")";
                stop();
                return false;
            }
            if (!impl_->out.back().openOut(t.ip, t.port, &oerr)) {
                if (err) *err = "open out failed for " + t.ip + ":" +
                                std::to_string(t.port) + " (" + oerr + ")";
                stop();
                return false;
            }
        }
        impl_->dests.push_back({t.ip, t.port});
    }
    received_ = forwarded_ = dropped_ = 0;
    started_ = true;
    return true;
}

void ProxyEngine::stop() {
    if (impl_) {
        impl_->listen.close();
        for (auto& o : impl_->out)
            o.close();
        impl_->out.clear();
        impl_->dests.clear();
    }
    started_ = false;
}

int ProxyEngine::pump() {
    if (!started_) return 0;
    int got = 0;
    OscMsg m;
    while (impl_->listen.poll(m, nullptr)) {
        ++got;
        ++received_;
        if (cfg_.mode == "off") {
            ++dropped_;
            continue;
        }
        if (cfg_.mode == "only-control") {
            bool hit = false;
            for (const auto& r : cfg_.routes) {
                if (r.srcAddr != m.addr) continue;
                const ProxyTarget& t = cfg_.targets[r.targetIdx];
                if (!t.enabled) continue;
                // Rewrite to the Heddle set-by-name form, forwarding the
                // surface's arguments verbatim (the value float, and any
                // delay/duration floats the surface sends).
                const std::string addr = "/chain/" + r.slot + "/" + r.param;
                OscMsg out = OscMsg::make(addr);
                for (float f : m.floats) {
                    out.floats.push_back(f);
                    out.typeTags.push_back('f');
                }
                for (int32_t i : m.ints) {
                    out.ints.push_back(i);
                    out.typeTags.push_back('i');
                }
                for (const auto& s : m.strs) {
                    out.strs.push_back(s);
                    out.typeTags.push_back('s');
                }
                if (impl_->out[r.targetIdx].send(out))
                    ++forwarded_;
                else
                    ++dropped_;
                hit = true;
            }
            if (!hit) ++dropped_;
            continue;
        }
        // mode == "full": forward the message as-is to every enabled target.
        bool sent = false;
        for (size_t i = 0; i < cfg_.targets.size(); ++i) {
            if (!cfg_.targets[i].enabled) continue;
            if (impl_->out[i].send(m))
                sent = true;
        }
        if (sent) ++forwarded_;
        else ++dropped_;
    }
    return got;
}

ProxyDiscovered ProxyEngine::discover(const ProxyTarget& target, int timeoutMs) {
    ProxyDiscovered res;
    if (target.ip.empty() || target.port <= 0)
        return res;  // ok=false

    // A dedicated socket pair for this probe (independent of the engine's
    // own listen/out sockets, so discovery works before start() and does not
    // disturb live forwarding). openOut requires a prior bind, so bind an
    // ephemeral local port for the out socket.
    OscUdp listen, out;
    std::string err;
    if (!listen.bind("127.0.0.1", 0, &err))
        return res;  // ephemeral port (0) -> OS picks one
    if (!out.bind("0.0.0.0", 0, &err))
        return res;
    if (!out.openOut(target.ip, target.port, &err))
        return res;

    // Send /chain/query and, once we know the slots, a per-slot param query.
    // Heddle's /reply/chain lists slots as "index<TAB>id<TAB>path<TAB>enabled";
    // we query each slot for its params (a bare /chain/<slot> with no args).
    out.send(OscMsg::make("/chain/query"));

    const auto t0 = std::chrono::steady_clock::now();
    const auto deadline = t0 + std::chrono::milliseconds(timeoutMs);
    std::vector<std::string> slotsToQuery;  // slot tokens not yet queried
    bool chainSeen = false;
    bool sentSlotQueries = false;

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now > deadline) break;
        const std::int64_t remainMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                .count();
        const int waitMs = (int) std::max(static_cast<std::int64_t>(1), remainMs);
        OscUdp::SourceAddr src;
        // Spin with a short sleep so the loop yields on a bare thread.
        // Replies are sent back to the SOURCE of our query — the `out`
        // socket's ephemeral port — so poll BOTH sockets (a well-behaved
        // target might also reply to the listen port).
        OscMsg m;
        bool gotMsg = false;
        const auto spinEnd = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(waitMs);
        while (std::chrono::steady_clock::now() < spinEnd) {
            if (out.poll(m, &src) || listen.poll(m, &src)) {
                gotMsg = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!gotMsg) continue;

        if (m.addr == "/reply/chain") {
            chainSeen = true;
            res.ok = true;
            // Heddle sends ONE string arg: the whole chain, lines separated by
            // '\n', each "index<TAB>id<TAB>path<TAB>enabled". Split on '\n'
            // first, then on '\t'.
            for (const auto& str : m.strs) {
                std::vector<std::string> lines;
                std::string cur;
                for (char ch : str) {
                    if (ch == '\n') { lines.push_back(cur); cur.clear(); }
                    else cur.push_back(ch);
                }
                lines.push_back(cur);
                for (const auto& line : lines) {
                    if (line.empty()) continue;
                    std::vector<std::string> cols;
                    std::string c;
                    for (char ch : line) {
                        if (ch == '\t') { cols.push_back(c); c.clear(); }
                        else c.push_back(ch);
                    }
                    cols.push_back(c);
                    // cols[1] is the slot id (= pluginId), the token that
                    // resolveSlot() accepts.
                    if (cols.size() >= 2 && !cols[1].empty())
                        slotsToQuery.push_back(cols[1]);
                }
            }
            // Issue a param query for each discovered slot, immediately.
            for (const auto& s : slotsToQuery)
                out.send(OscMsg::make("/chain/" + s));
            sentSlotQueries = !slotsToQuery.empty();
            continue;
        }
        if (m.addr.rfind("/reply/", 0) == 0) {
            // /reply/<slotId>/<paramTitle> with a STRING arg = one param line
            // "id<TAB>title<TAB>value<TAB>min<TAB>max<TAB>steps<TAB>kind...".
            // Error replies ("error: ...", "(no params)") also arrive as
            // /reply/<slotId> strings but have NO tab and col[0] is not a
            // numeric param id — require both to filter them out.
            const std::string rest = m.addr.substr(7);
            const size_t slash = rest.find('/');
            const std::string slot = (slash == std::string::npos) ? rest : rest.substr(0, slash);
            for (const auto& line : m.strs) {
                std::vector<std::string> cols;
                std::string cur;
                for (char ch : line) {
                    if (ch == '\t') { cols.push_back(cur); cur.clear(); }
                    else cur.push_back(ch);
                }
                cols.push_back(cur);
                // Need >=2 tab-cols, and col[0] must be a non-empty integer.
                if (cols.size() < 2) continue;
                bool numericId = !cols[0].empty();
                for (char ch : cols[0])
                    if (ch < '0' || ch > '9') { numericId = false; break; }
                if (!numericId) continue;
                int pid = 0;
                for (char ch : cols[0]) pid = pid * 10 + (ch - '0');
                res.params.push_back({slot, cols[1], pid});
            }
        }
        // /reply/<slot>/<param> with a FLOAT arg is a set-ack — ignore.
    }
    (void) sentSlotQueries;
    // If the target never answered /reply/chain, res.ok stays false.
    return res;
}

}  // namespace heddle
