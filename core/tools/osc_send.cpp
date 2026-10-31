// heddle-osc-send — a test connector for Heddle's OSC receive path.
//
// Emits OSC messages the way a control surface would, to prove the pipeline
// end to end. Two wire forms:
//   --bare       plain OSC 1.0 datagram, NO length header — exactly what
//                TouchOSC / Ableton Live / Max / TouchDesigner emit. This is
//                the form the proxy must accept (regression guard for the
//                "TouchOSC never reaches the proxy" bug).
//   --prefixed   (default) 4-byte big-endian length header + OSC 1.0 —
//                Heddle's internal framing (oscEncode).
//
// Usage:
//   heddle-osc-send <ip:port> [--addr /f1] [--float 0.5 [f2]] [--int N]
//                       [--str S] [--bare] [--count 5] [--interval 100]
//
// Exit 0 = all sent; 1 = usage; 2 = socket error.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "heddle/osc.hpp"
#include "heddle/osc_io.hpp"

using namespace heddle;

namespace {

// OSC 1.0 body with NO length header (what external surfaces actually put
// on the wire). oscEncode() appends the 4-byte header, so build the body
// directly.
std::string bareOscBody(const OscMsg& m) {
    std::string s;
    auto pad4 = [](std::string& s) {
        const size_t r = s.size() & size_t(3);
        if (r) s.append(4u - r, '\0');
    };
    s += m.addr;
    s.push_back('\0');
    pad4(s);
    std::string tags = ",";
    for (char t : m.typeTags) tags.push_back(t);
    tags.push_back('\0');
    s += tags;
    pad4(s);
    size_t fi = 0, ii = 0, si = 0;
    for (char t : m.typeTags) {
        switch (t) {
        case 'f': { uint32_t u; std::memcpy(&u, &m.floats[fi++], 4);
            for (int s2 = 24; s2 >= 0; s2 -= 8) s += char((u >> s2) & 0xff);
            break; }
        case 'i': { uint32_t u = uint32_t(m.ints[ii++]);
            for (int s2 = 24; s2 >= 0; s2 -= 8) s += char((u >> s2) & 0xff);
            break; }
        case 's': case 'b': { const auto& b = m.strs[si++];
            if (t == 'b') { uint32_t u = uint32_t(b.size());
                for (int s2 = 24; s2 >= 0; s2 -= 8) s += char((u >> s2) & 0xff); }
            s += b; s.push_back('\0'); pad4(s); break; }
        default: break;
        }
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    std::string ipPort, addr = "/f1";
    std::vector<float> floats;
    bool hasInt = false; int32_t iarg = 0;
    bool hasStr = false; std::string sarg;
    bool bare = false;
    int count = 5, intervalMs = 100;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&](int& i, const char*) -> const char* {
            if (i + 1 >= argc) return nullptr;
            return argv[++i];
        };
        if (a == "--bare") bare = true;
        else if (a == "--prefixed") bare = false;
        else if (a == "--addr") { const char* v = val(i, "--addr"); if (!v) return 1; addr = v; }
        else if (a == "--float") { const char* v = val(i, "--float"); if (!v) return 1; floats.push_back(std::atof(v)); }
        else if (a == "--int") { const char* v = val(i, "--int"); if (!v) return 1; hasInt = true; iarg = std::atoi(v); }
        else if (a == "--str") { const char* v = val(i, "--str"); if (!v) return 1; hasStr = true; sarg = v; }
        else if (a == "--count") { const char* v = val(i, "--count"); if (!v) return 1; count = std::atoi(v); }
        else if (a == "--interval") { const char* v = val(i, "--interval"); if (!v) return 1; intervalMs = std::atoi(v); }
        else if (a[0] != '-') {
            if (!ipPort.empty()) { std::fprintf(stderr, "error: extra argument '%s'\n", a.c_str()); return 1; }
            ipPort = a;
        } else { std::fprintf(stderr, "error: unknown flag '%s'\n", a.c_str()); return 1; }
    }
    if (ipPort.empty()) {
        std::fprintf(stderr,
            "usage: heddle-osc-send <ip:port> [--addr /f1] [--float 0.5] "
            "[--int N] [--str S] [--bare] [--count 5] [--interval 100]\n");
        return 1;
    }
    if (floats.empty() && !hasInt && !hasStr) floats.push_back(0.5f);  // default like a fader
    const size_t p = ipPort.rfind(':');
    if (p == std::string::npos || p + 1 >= ipPort.size()) {
        std::fprintf(stderr, "error: expected ip:port\n");
        return 1;
    }
    const std::string ip = ipPort.substr(0, p);
    int port = std::atoi(ipPort.substr(p + 1).c_str());
    if (port <= 0 || port > 65535) { std::fprintf(stderr, "error: bad port\n"); return 1; }

    OscMsg m;
    m.addr = addr;
    if (hasInt) { m.typeTags.push_back('i'); m.ints.push_back(iarg); }
    for (float f : floats) { m.typeTags.push_back('f'); m.floats.push_back(f); }
    if (hasStr) { m.typeTags.push_back('s'); m.strs.push_back(sarg); }

    const std::string wire = bare ? bareOscBody(m) : oscEncode(m);

    OscUdp u;
    std::string err;
    if (!u.bind("0.0.0.0", 0, &err)) { std::fprintf(stderr, "bind: %s\n", err.c_str()); return 2; }
    if (!u.openOut(ip, port, &err)) {
        std::fprintf(stderr, "openOut %s:%d: %s\n", ip.c_str(), port, err.c_str());
        return 2;
    }
    for (int n = 0; n < count; ++n) {
        if (!u.sendRaw(wire)) {
            std::fprintf(stderr, "sendto failed (n=%d)\n", n);
            return 2;
        }
        if (n + 1 < count) std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }
    std::fprintf(stderr, "sent %d x %zu-byte %s OSC to %s:%d  addr=%s\n",
                 count, wire.size(), bare ? "bare (TouchOSC form)" : "length-prefixed (Heddle form)",
                 ip.c_str(), port, addr.c_str());
    return 0;
}
