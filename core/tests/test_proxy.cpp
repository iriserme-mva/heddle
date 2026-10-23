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
