// Local UDP round-trip tests for OscUdp: bind/poll, source-address capture,
// and sendTo() reply routing. Uses 127.0.0.1 with ephemeral ports only —
// no fixed ports, no network, safe in CI sandboxes.
#include "doctest.h"

#include "heddle/osc.hpp"
#include "heddle/osc_io.hpp"

#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
static void sleepMs(int ms) { ::Sleep(uint32_t(ms)); }
#else
#include <unistd.h>
static void sleepMs(int ms) { ::usleep(useconds_t(ms * 1000)); }
#endif

using namespace heddle;

namespace {
// poll() with a bounded retry window (UDP loopback is fast but not
// guaranteed to be same-tick). Returns true if a message was received.
bool pollWithRetry(OscUdp& u, OscMsg& out, OscUdp::SourceAddr* src,
                   int maxTries = 200) {
    for (int i = 0; i < maxTries; ++i) {
        if (u.poll(out, src)) return true;
        sleepMs(5);
    }
    return false;
}
}  // namespace

TEST_SUITE("osc.udp")
{

TEST_CASE("udp loopback roundtrip with source capture and sendTo reply") {
    std::string err;
    OscUdp server;
    CHECK(server.bind("127.0.0.1", 0, &err));          // ephemeral port
    const int serverPort = server.localPort();
    REQUIRE(serverPort > 0);

    OscUdp client;
    CHECK(client.bind("127.0.0.1", 0, &err));
    const int clientPort = client.localPort();
    REQUIRE(clientPort > 0);
    CHECK(client.openOut("127.0.0.1", serverPort, &err));

    // Client -> server.
    CHECK(client.send(OscMsg::make("/chain/comp/Threshold", 0.42f)));
    OscMsg rx;
    OscUdp::SourceAddr src;
    src.ip = "";
    CHECK(pollWithRetry(server, rx, &src));
    CHECK(rx.addr == "/chain/comp/Threshold");
    CHECK(rx.floats.size() == 1);
    CHECK(std::abs(rx.floats[0] - 0.42f) < 1e-6f);
    // Source must be the client's own bound address — this is what the host
    // uses to route replies back to the asking client.
    CHECK(src.ip == "127.0.0.1");
    CHECK(src.port == clientPort);

    // Server -> client via sendTo() to the captured source.
    CHECK(server.sendTo(OscMsg::make("/reply/comp/Threshold", 0.42f), src));
    OscMsg rx2;
    CHECK(pollWithRetry(client, rx2, nullptr));
    CHECK(rx2.addr == "/reply/comp/Threshold");
    CHECK(std::abs(rx2.floats[0] - 0.42f) < 1e-6f);

    // No more pending: poll must be clean (non-blocking, no leftover datagrams).
    OscMsg rx3;
    CHECK_FALSE(client.poll(rx3, nullptr));
}

TEST_CASE("poll returns false on a bound-but-quiet socket") {
    std::string err;
    OscUdp u;
    CHECK(u.bind("127.0.0.1", 0, &err));
    OscMsg m;
    CHECK_FALSE(u.poll(m, nullptr));
    CHECK_FALSE(u.poll(m, nullptr));
}

TEST_CASE("poll overload without source arg still works") {
    std::string err;
    OscUdp server;
    CHECK(server.bind("127.0.0.1", 0, &err));
    OscUdp client;
    CHECK(client.bind("127.0.0.1", 0, &err));
    CHECK(client.openOut("127.0.0.1", server.localPort(), &err));
    CHECK(client.send(OscMsg::make("/chain/query", std::string(""))));
    OscMsg rx;
    CHECK(pollWithRetry(server, rx, nullptr));
    CHECK(rx.addr == "/chain/query");
}

}  // namespace
TEST_SUITE_END();
