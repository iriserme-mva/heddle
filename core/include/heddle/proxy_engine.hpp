#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "heddle/proxy.hpp"
#include "heddle/osc.hpp"
#include "heddle/osc_io.hpp"

namespace heddle {

// ---------------------------------------------------------------------------
// ProxyEngine — the OSC fan-out router shared by the passthrough VST3 and
// the standalone heddle-proxy executable (see docs/PROXY.md).
//
// Threading model: ONE owner thread. start() binds the listen socket; the
// owner calls pump() on a short timer (the VST3 editor runs a juce::Timer at
// ~200 Hz; the standalone runs its own loop). pump() is non-blocking — it
// drains whatever arrived and forwards it. This keeps the engine free of its
// own threads/locks, so it is safe to drive from JUCE's message thread and
// trivially testable from a plain unit test.
//
// Forwarding is fire-and-forget UDP to each enabled target (OscUdp::sendTo
// with a fixed dest per target). There is no reply tracking on the forward
// path: the proxy does NOT echo target replies back to the surface (that
// would loop a surface that also listens on its own output). See the
// "ack-back" note in docs/PROXY.md.
// ---------------------------------------------------------------------------
class ProxyEngine {
public:
    ProxyEngine();
    ~ProxyEngine();

    // Apply a config: (re)bind the listen socket, open one output socket per
    // enabled target, rebuild the routing map. Returns false + *err if the
    // config is invalid or a socket fails to open. Idempotent: calling while
    // already started re-binds cleanly (the old sockets are closed first).
    bool start(const ProxyConfig& cfg, std::string* err);
    // Close every socket, drop the routing map. Safe to call when not started.
    void stop();

    // Drain the listen socket and forward each received message per the
    // current mode/routing map. Non-blocking. Returns the number of messages
    // received (0 = nothing). Call from the owner thread only.
    int pump();

    // OSC discovery: query `target` (ip:port) for its chain + per-slot param
    // list (docs/OSC.md) and wait up to `timeoutMs` for the replies. Runs on
    // the calling thread (blocks the message thread for at most timeoutMs —
    // the editor shows a "discovering…" state). Returns ok=true only if a
    // /reply/chain arrived; params are filled from /reply/<slot> lines.
    ProxyDiscovered discover(const ProxyTarget& target, int timeoutMs);

    bool running() const { return started_; }
    const ProxyConfig& config() const { return cfg_; }
    // Live counters (owner-thread reads; pump() writes).
    long long received() const { return received_; }
    long long forwarded() const { return forwarded_; }
    long long dropped() const { return dropped_; }

    // Re-read the routing table from cfg_ after a start(). Public for tests.
    // A message matches a route when its address EXACTLY equals route.srcAddr.
    // (Address normalization — trimming a leading "/k/" to the control token —
    // is intentionally NOT done here: TouchOSC address layouts vary, so the
    // user writes the exact address each surface sends.)
    struct RouteKey {
        std::string srcAddr;
        std::vector<int> targetIdx;  // one or more targets share a source
    };
    const std::vector<RouteKey>& routes() const { return routeKeys_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;

    ProxyConfig cfg_;
    bool started_ = false;

    // srcAddr -> the (possibly several) target indices that rule routes it.
    std::vector<RouteKey> routeKeys_;

    // Counters. pump() is single-owner, so plain writes/reads with no lock.
    long long received_ = 0;
    long long forwarded_ = 0;
    long long dropped_ = 0;
};

}  // namespace heddle
