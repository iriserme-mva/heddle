// heddle-proxy tests: config parse/validate/dump + live UDP routing
// (only-control rewrite, off drop) + discovery parsing, over loopback.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "heddle/proxy.hpp"
#include "heddle/proxy_engine.hpp"
#include "heddle/osc_io.hpp"

using namespace heddle;

namespace {

// A minimal fake Heddle target: binds a UDP socket and answers the Heddle
// OSC protocol (docs/OSC.md) with the EXACT reply formats the real host uses:
//   /chain/query     -> /reply/chain  (ONE string; '\n'-separated lines)
//   /chain/<slotId>  -> /reply/<slotId>/<title>  (one string per param,
//                        "id<TAB>title<TAB>value...")
class FakeTarget {
public:
    bool start(int port) {
        std::string err;
        return sock_.bind("127.0.0.1", port, &err);
    }
    // Drain pending; answer /chain/query, /chain/comp, /chain/delay.
    void serve() {
        OscMsg m;
        OscUdp::SourceAddr src;
        while (sock_.poll(m, &src)) {
            if (m.addr == "/chain/query") {
                OscMsg r = OscMsg::make("/reply/chain");
                // One string: index<TAB>id<TAB>path<TAB>enabled  (two slots)
                r.strs.push_back(
                    "0\tcomp\t/fake/comp.vst3\ttrue\n"
                    "1\tdelay\t/fake/delay.vst3\ttrue");
                r.typeTags.push_back('s');
                sock_.sendTo(r, src);
            } else if (m.addr == "/chain/comp") {
                sock_.sendTo(
                    OscMsg::make("/reply/comp/Gain", "1\tGain\t0.5\t0\t1\t0\tfloat"),
                    src);
                sock_.sendTo(
                    OscMsg::make("/reply/comp/Cutoff", "2\tCutoff\t0.5\t0\t1\t0\tfloat"),
                    src);
            } else if (m.addr == "/chain/delay") {
                // A slot with no params: the "(no params)" error string.
                sock_.sendTo(OscMsg::make("/reply/delay", "(no params)"), src);
            }
        }
    }
    // Non-blocking single poll (for the routing test).
    bool pollMsg(OscMsg& m) {
        OscUdp::SourceAddr src;
        return sock_.poll(m, &src);
    }
private:
    OscUdp sock_;
};

}  // namespace

TEST_CASE("proxy config parse/validate/dump round-trip") {
    std::string err;
    ProxyConfig c;
    CHECK(proxy::parse(R"({
        "listen_port": 9100,
        "listen_host": "127.0.0.1",
        "mode": "only-control",
        "targets": [
            {"ip": "127.0.0.1", "port": 9001},
            {"ip": "127.0.0.1", "port": 9002, "enabled": false}
        ],
        "routes": [
            {"src": "/k/0", "target": 0, "slot": "comp", "param": "Gain"}
        ]
    })", c, &err));
    CHECK(c.listenPort == 9100);
    CHECK(c.mode == "only-control");
    CHECK(c.targets.size() == 2);
    CHECK(!c.targets[1].enabled);
    CHECK(c.routes.size() == 1);
    CHECK(c.routes[0].srcAddr == "/k/0");
    CHECK(c.routes[0].param == "Gain");

    ProxyConfig c2;
    CHECK(proxy::parse(proxy::dump(c), c2, &err));
    CHECK(proxy::dump(c2) == proxy::dump(c));

    ProxyConfig bad;
    bad.listenPort = 0;
    CHECK_FALSE(ProxyConfig::validate(bad, &err));
    bad.listenPort = 9100;
    bad.mode = "maybe";
    CHECK_FALSE(ProxyConfig::validate(bad, &err));
    bad.mode = "full";
    ProxyTarget t; t.ip = "127.0.0.1"; t.port = 9001;
    for (int i = 0; i < 6; ++i) bad.targets.push_back(t);
    CHECK_FALSE(ProxyConfig::validate(bad, &err));
    bad.targets.pop_back();
    bad.routes.push_back({"/k/1", 9, "s", "p"});
    CHECK_FALSE(ProxyConfig::validate(bad, &err));
    CHECK(err.find("target index") != std::string::npos);
}

TEST_CASE("proxy routes only-control over UDP (rewrite + drop)") {
    constexpr int listenPort = 42101;
    constexpr int targetPort = 42102;
    FakeTarget target;
    REQUIRE(target.start(targetPort));

    ProxyConfig c;
    c.listenPort = listenPort;
    c.listenHost = "127.0.0.1";
    c.mode = "only-control";
    c.targets.push_back({"127.0.0.1", targetPort, true});
    c.routes.push_back({"/k/0", 0, "comp", "Gain"});

    ProxyEngine engine;
    std::string err;
    REQUIRE(engine.start(c, &err));

    OscUdp surface;
    REQUIRE(surface.bind("0.0.0.0", 0, &err));
    REQUIRE(surface.openOut("127.0.0.1", listenPort, &err));
    surface.send(OscMsg::make("/k/0", 0.6f));   // routed
    surface.send(OscMsg::make("/k/1", 0.3f));   // unrouted -> dropped

    int got = 0;
    for (int i = 0; i < 400 && got < 2; ++i) got += engine.pump();
    CHECK(got == 2);
    CHECK(engine.received() == 2);
    CHECK(engine.forwarded() == 1);
    CHECK(engine.dropped() == 1);

    OscMsg m;
    bool saw = false;
    for (int i = 0; i < 400 && !saw; ++i) if (target.pollMsg(m)) saw = true;
    REQUIRE(saw);
    CHECK(m.addr == "/chain/comp/Gain");
    CHECK(m.floats.size() == 1);
    CHECK(m.floats[0] == doctest::Approx(0.6f));
    engine.stop();
}

TEST_CASE("proxy off mode drops everything") {
    ProxyConfig c;
    c.listenPort = 42103;
    c.listenHost = "127.0.0.1";
    c.mode = "off";
    c.targets.push_back({"127.0.0.1", 42104, true});
    ProxyEngine engine;
    std::string err;
    REQUIRE(engine.start(c, &err));
    OscUdp surface;
    REQUIRE(surface.bind("0.0.0.0", 0, &err));
    REQUIRE(surface.openOut("127.0.0.1", 42103, &err));
    surface.send(OscMsg::make("/chain/comp/Gain", 0.9f));
    int got = 0;
    for (int i = 0; i < 400 && got == 0; ++i) got += engine.pump();
    CHECK(got == 1);
    CHECK(engine.forwarded() == 0);
    CHECK(engine.dropped() == 1);
    engine.stop();
}

TEST_CASE("proxy curve eval: shapes, clamping, name round-trip") {
    // Identity / linear.
    CHECK(proxyCurveEval(ProxyCurve::identity, 0.25f) == doctest::Approx(0.25f));
    CHECK(proxyCurveEval(ProxyCurve::linear, 0.75f) == doctest::Approx(0.75f));
    // exp = x^2.
    CHECK(proxyCurveEval(ProxyCurve::exp, 0.5f) == doctest::Approx(0.25f));
    CHECK(proxyCurveEval(ProxyCurve::exp, 0.2f) == doctest::Approx(0.04f));
    // invexp = 1 - (1-x)^2.
    CHECK(proxyCurveEval(ProxyCurve::invexp, 0.5f) == doctest::Approx(0.75f));
    CHECK(proxyCurveEval(ProxyCurve::invexp, 0.2f) == doctest::Approx(0.36f));
    // scurve = 3x^2 - 2x^3 (smoothstep): midpoint fixed, monotonic, kink-free.
    CHECK(proxyCurveEval(ProxyCurve::scurve, 0.0f) == doctest::Approx(0.0f));
    CHECK(proxyCurveEval(ProxyCurve::scurve, 0.5f) == doctest::Approx(0.5f));
    CHECK(proxyCurveEval(ProxyCurve::scurve, 1.0f) == doctest::Approx(1.0f));
    CHECK(proxyCurveEval(ProxyCurve::scurve, 0.25f) < 0.25f);   // slow start
    CHECK(proxyCurveEval(ProxyCurve::scurve, 0.75f) > 0.75f);   // fast end
    // Clamping at both ends.
    CHECK(proxyCurveEval(ProxyCurve::exp, -1.0f) == doctest::Approx(0.0f));
    CHECK(proxyCurveEval(ProxyCurve::scurve, 2.0f) == doctest::Approx(1.0f));

    // Name round-trip (eval shapes themselves are checked above).
    const char* names[] = {"identity", "linear", "exp", "invexp", "scurve"};
    for (const char* n : names) {
        ProxyCurve c;
        REQUIRE(proxyCurveFromName(n, &c));
        CHECK(std::string(proxyCurveName(c)) == n);
    }
    ProxyCurve dummy;
    CHECK_FALSE(proxyCurveFromName("cosine", &dummy));
    CHECK_FALSE(proxyCurveFromName("", &dummy));
}

TEST_CASE("proxy config parse/validate: mapping fields round-trip + defaults") {
    std::string err;
    ProxyConfig c;
    CHECK(proxy::parse(R"({
        "listen_port": 9100,
        "listen_host": "127.0.0.1",
        "mode": "only-control",
        "targets": [{"ip": "127.0.0.1", "port": 9001}],
        "routes": [
            {"src": "/xy", "target": 0, "slot": "comp", "param": "Gain",
             "arg": 1, "in_min": -1.0, "in_max": 1.0, "curve": "exp"}
        ]
    })", c, &err));
    REQUIRE(c.routes.size() == 1);
    CHECK(c.routes[0].arg == 1);
    CHECK(c.routes[0].inMin == doctest::Approx(-1.0));
    CHECK(c.routes[0].inMax == doctest::Approx(1.0));
    CHECK(c.routes[0].curve == ProxyCurve::exp);

    // dump includes the new keys; re-parsing is stable.
    ProxyConfig c2;
    CHECK(proxy::parse(proxy::dump(c), c2, &err));
    CHECK(proxy::dump(c2) == proxy::dump(c));
    CHECK(c2.routes[0].arg == 1);
    CHECK(c2.routes[0].curve == ProxyCurve::exp);

    // Old config (no mapping keys) parses with identity defaults.
    ProxyConfig old;
    CHECK(proxy::parse(R"({
        "listen_port": 9100, "listen_host": "127.0.0.1",
        "mode": "only-control",
        "targets": [{"ip": "127.0.0.1", "port": 9001}],
        "routes": [{"src": "/k/0", "target": 0, "slot": "s", "param": "p"}]
    })", old, &err));
    REQUIRE(old.routes.size() == 1);
    CHECK(old.routes[0].arg == 0);
    CHECK(old.routes[0].inMin == doctest::Approx(0.0));
    CHECK(old.routes[0].inMax == doctest::Approx(1.0));
    CHECK(old.routes[0].curve == ProxyCurve::identity);

    // Validation of the new fields (fresh config each time, so each failure
    // is reported for the field under test).
    ProxyConfig bad;
    bad.listenPort = 9100;
    bad.listenHost = "127.0.0.1";
    bad.mode = "only-control";
    ProxyTarget t; t.ip = "127.0.0.1"; t.port = 9001;
    bad.targets.push_back(t);
    ProxyRoute r; r.srcAddr = "/a"; r.targetIdx = 0; r.slot = "s"; r.param = "p";
    r.arg = -1;
    bad.routes.push_back(r);
    CHECK_FALSE(ProxyConfig::validate(bad, &err));
    CHECK(err.find("arg") != std::string::npos);
    bad.routes.clear();
    r.arg = 0;
    r.inMin = 1.0; r.inMax = 0.0;  // reversed range = inverted mapping: allowed
    bad.routes.push_back(r);
    CHECK(ProxyConfig::validate(bad, &err));
}

TEST_CASE("proxy route mapping: arg pick + rescale + curve over UDP") {
    constexpr int listenPort = 42111;
    constexpr int targetPort = 42112;
    FakeTarget target;
    REQUIRE(target.start(targetPort));

    // Route: pick float[1] (the "y" of an X/Y knob), rescale [-1,1] -> [0,1],
    // shape with exp.
    ProxyConfig c;
    c.listenPort = listenPort;
    c.listenHost = "127.0.0.1";
    c.mode = "only-control";
    c.targets.push_back({"127.0.0.1", targetPort, true});
    ProxyRoute r;
    r.srcAddr = "/xy"; r.targetIdx = 0; r.slot = "comp"; r.param = "Gain";
    r.arg = 1; r.inMin = -1.0; r.inMax = 1.0; r.curve = ProxyCurve::exp;
    c.routes.push_back(r);

    ProxyEngine engine;
    std::string err;
    REQUIRE(engine.start(c, &err));

    OscUdp surface;
    REQUIRE(surface.bind("0.0.0.0", 0, &err));
    REQUIRE(surface.openOut("127.0.0.1", listenPort, &err));
    // X=0.1 (ignored), Y=0.5 -> x=(0.5-(-1))/2=0.75 -> exp=0.5625
    surface.send(OscMsg::make("/xy", std::vector<float>{0.1f, 0.5f}));
    // Y=-0.5 -> x=0.25 -> exp=0.0625
    surface.send(OscMsg::make("/xy", std::vector<float>{0.9f, -0.5f}));
    // Y=3.0 (out of range) -> clamps to x=1 -> exp=1
    surface.send(OscMsg::make("/xy", std::vector<float>{0.0f, 3.0f}));

    int got = 0;
    for (int i = 0; i < 400 && got < 3; ++i) got += engine.pump();
    CHECK(got == 3);
    CHECK(engine.forwarded() == 3);

    float seen[3] = {0, 0, 0};
    int n = 0;
    OscMsg m;
    for (int i = 0; i < 400 && n < 3; ++i) {
        if (!target.pollMsg(m)) continue;
        CHECK(m.addr == "/chain/comp/Gain");
        // Exactly ONE float is forwarded — the picked, mapped value. The
        // surface's extra float must NOT leak in as a "delay".
        CHECK(m.floats.size() == 1);
        if (n < 3) seen[n] = m.floats[0];
        ++n;
    }
    CHECK(n == 3);
    CHECK(seen[0] == doctest::Approx(0.5625f));
    CHECK(seen[1] == doctest::Approx(0.0625f));
    CHECK(seen[2] == doctest::Approx(1.0f));
    engine.stop();
}

TEST_CASE("proxy route mapping: inverted range flips the value") {
    constexpr int listenPort = 42120;
    constexpr int targetPort = 42121;
    FakeTarget target;
    REQUIRE(target.start(targetPort));

    ProxyConfig c;
    c.listenPort = listenPort;
    c.listenHost = "127.0.0.1";
    c.mode = "only-control";
    c.targets.push_back({"127.0.0.1", targetPort, true});
    ProxyRoute r;
    r.srcAddr = "/rev"; r.targetIdx = 0; r.slot = "comp"; r.param = "Gain";
    r.inMin = 1.0; r.inMax = 0.0;  // inverted: 0 -> 1, 1 -> 0, 0.5 -> 0.5
    c.routes.push_back(r);

    ProxyEngine engine;
    std::string err;
    REQUIRE(engine.start(c, &err));
    OscUdp surface;
    REQUIRE(surface.bind("0.0.0.0", 0, &err));
    REQUIRE(surface.openOut("127.0.0.1", listenPort, &err));
    surface.send(OscMsg::make("/rev", 0.0f));
    surface.send(OscMsg::make("/rev", 0.5f));
    surface.send(OscMsg::make("/rev", 1.0f));

    int got = 0;
    for (int i = 0; i < 400 && got < 3; ++i) got += engine.pump();
    CHECK(got == 3);

    float seen[3] = {0, 0, 0};
    int n = 0;
    OscMsg m;
    for (int i = 0; i < 400 && n < 3; ++i) {
        if (!target.pollMsg(m)) continue;
        if (n < 3) seen[n] = m.floats.front();
        ++n;
    }
    CHECK(n == 3);
    CHECK(seen[0] == doctest::Approx(1.0f));
    CHECK(seen[1] == doctest::Approx(0.5f));
    CHECK(seen[2] == doctest::Approx(0.0f));
    engine.stop();
}

TEST_CASE("proxy route mapping: arg out of range is dropped for that route") {
    constexpr int listenPort = 42113;
    constexpr int targetPort = 42114;
    FakeTarget target;
    REQUIRE(target.start(targetPort));

    ProxyConfig c;
    c.listenPort = listenPort;
    c.listenHost = "127.0.0.1";
    c.mode = "only-control";
    c.targets.push_back({"127.0.0.1", targetPort, true});
    ProxyRoute r;
    r.srcAddr = "/s"; r.targetIdx = 0; r.slot = "comp"; r.param = "Gain";
    r.arg = 1;  // wants a second float
    c.routes.push_back(r);

    ProxyEngine engine;
    std::string err;
    REQUIRE(engine.start(c, &err));
    OscUdp surface;
    REQUIRE(surface.bind("0.0.0.0", 0, &err));
    REQUIRE(surface.openOut("127.0.0.1", listenPort, &err));
    surface.send(OscMsg::make("/s", 0.5f));  // only one float -> dropped

    int got = 0;
    for (int i = 0; i < 400 && got < 1; ++i) got += engine.pump();
    CHECK(got == 1);
    CHECK(engine.forwarded() == 0);
    CHECK(engine.dropped() == 1);
    OscMsg m;
    bool saw = false;
    for (int i = 0; i < 200 && !saw; ++i) if (target.pollMsg(m)) saw = true;
    CHECK(!saw);  // nothing reached the target
    engine.stop();
}

TEST_CASE("proxy same input fans out to multiple targets via multiple routes") {
    constexpr int listenPort = 42115;
    constexpr int targetPortA = 42116;
    constexpr int targetPortB = 42117;
    FakeTarget ta, tb;
    REQUIRE(ta.start(targetPortA));
    REQUIRE(tb.start(targetPortB));

    ProxyConfig c;
    c.listenPort = listenPort;
    c.listenHost = "127.0.0.1";
    c.mode = "only-control";
    c.targets.push_back({"127.0.0.1", targetPortA, true});
    c.targets.push_back({"127.0.0.1", targetPortB, true});
    // One surface control -> TWO routes (two different targets).
    c.routes.push_back({"/k/0", 0, "comp", "Gain"});
    ProxyRoute r2;
    r2.srcAddr = "/k/0"; r2.targetIdx = 1; r2.slot = "comp"; r2.param = "Cutoff";
    r2.curve = ProxyCurve::invexp;
    c.routes.push_back(r2);

    ProxyEngine engine;
    std::string err;
    REQUIRE(engine.start(c, &err));
    OscUdp surface;
    REQUIRE(surface.bind("0.0.0.0", 0, &err));
    REQUIRE(surface.openOut("127.0.0.1", listenPort, &err));
    surface.send(OscMsg::make("/k/0", 0.5f));

    int got = 0;
    for (int i = 0; i < 400 && got < 1; ++i) got += engine.pump();
    CHECK(got == 1);
    CHECK(engine.forwarded() == 2);   // one message, two targets

    OscMsg m;
    bool sawA = false, sawB = false;
    float vA = 0.f, vB = 0.f;
    for (int i = 0; i < 400 && !(sawA && sawB); ++i) {
        if (ta.pollMsg(m)) { sawA = true; vA = m.floats.front(); }
        else if (tb.pollMsg(m)) { sawB = true; vB = m.floats.front(); }
    }
    REQUIRE(sawA);
    REQUIRE(sawB);
    CHECK(vA == doctest::Approx(0.5f));       // identity
    CHECK(vB == doctest::Approx(0.75f));      // invexp(0.5) = 1 - 0.25
    engine.stop();
}

TEST_CASE("proxy learn list: distinct addresses, newest first, arg counts") {
    constexpr int listenPort = 42118;
    ProxyConfig c;
    c.listenPort = listenPort;
    c.listenHost = "127.0.0.1";
    c.mode = "only-control";  // nothing routed — the learn list is still filled
    ProxyEngine engine;
    std::string err;
    REQUIRE(engine.start(c, &err));
    OscUdp surface;
    REQUIRE(surface.bind("0.0.0.0", 0, &err));
    REQUIRE(surface.openOut("127.0.0.1", listenPort, &err));
    surface.send(OscMsg::make("/a", 0.1f));
    surface.send(OscMsg::make("/b", std::vector<float>{0.2f, 0.3f}));
    surface.send(OscMsg::make("/a", 0.9f));  // repeat: refreshes /a, moves to top

    int got = 0;
    for (int i = 0; i < 400 && got < 3; ++i) got += engine.pump();
    CHECK(got == 3);

    const auto& rec = engine.recent();
    REQUIRE(rec.size() == 2);
    CHECK(rec[0].addr == "/a");       // most recent
    CHECK(rec[0].numFloats == 1);
    CHECK(rec[0].first == doctest::Approx(0.9f));
    CHECK(rec[1].addr == "/b");
    CHECK(rec[1].numFloats == 2);
    CHECK(rec[1].first == doctest::Approx(0.2f));
    engine.stop();
}

TEST_CASE("proxy discovery parses chain + params, filters errors") {
    constexpr int targetPort = 42105;
    FakeTarget target;
    REQUIRE(target.start(targetPort));

    ProxyEngine engine;  // not started — discovery is independent of start()
    ProxyTarget t; t.ip = "127.0.0.1"; t.port = targetPort;

    // discover() blocks while sleeping 2ms between polls, so the fake must be
    // pumped from a second thread to keep answering.
    std::atomic<bool> run{true};
    std::thread fake([&] {
        while (run) {
            target.serve();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    ProxyDiscovered d = engine.discover(t, 800);
    run = false;
    fake.join();

    CHECK(d.ok);
    int compCount = 0;
    bool haveGain = false;
    for (const auto& p : d.params) {
        if (p.slot == "comp") {
            ++compCount;
            if (p.param == "Gain") haveGain = (p.paramId == 1);
        }
    }
    CHECK(compCount == 2);   // "(no params)" for slot "delay" filtered out
    CHECK(haveGain);
}
