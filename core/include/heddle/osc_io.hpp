#pragma once
#include <string>

#include "heddle/osc.hpp"

namespace heddle {

// UDP OSC transport (OSC over UDP — same framing over TCP works too).
//
// Bind() listens on host:port (use "0.0.0.0" or "127.0.0.1");
// openOut() only sends to a remote host:port. poll() is non-blocking
// (0 ms timeout) — the host calls it from its timer/heartbeat.
//
// Source tracking: the poll() overload with a SourceAddr* records the
// sender (ip + port) of the last packet, so a host can route replies back
// to the client that asked (docs/OSC.md: "sends replies to the client's
// source address"). sendTo() replies to an explicit address, e.g. the one
// captured by poll().
class OscUdp {
public:
    // Sender of the most recently received packet (host-byte-order port).
    struct SourceAddr {
        std::string ip;
        int port = 0;
    };

    OscUdp();
    ~OscUdp();

    bool bind(const std::string& host, int port, std::string* err);
    bool openOut(const std::string& host, int port, std::string* err);

    bool send(const OscMsg& msg);
    // Returns false if nothing available. If src is non-null, it receives
    // the sender address of the packet.
    bool poll(OscMsg& out, SourceAddr* src = nullptr);
    // Send to an explicit destination (used to reply to a poll() source).
    bool sendTo(const OscMsg& msg, const SourceAddr& dest);
    void close();

    int localPort() const { return localPort_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    int localPort_ = 0;
};

}  // namespace heddle
