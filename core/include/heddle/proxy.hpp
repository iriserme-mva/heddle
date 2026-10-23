#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace heddle {

// ---------------------------------------------------------------------------
// heddle-proxy — OSC fan-out router (see docs/PROXY.md)
//
// Routes one control surface (e.g. TouchOSC) to up to N Heddle instances:
//   surface  --OSC-->  proxy  --OSC-->  target 1..N
//
// The engine is JUCE-free (links only heddle_core's OSC codec/transport) so
// the same code runs inside the passthrough VST3 (editor UI + audio thread)
// and the standalone heddle-proxy executable. Audio passthrough and any GUI
// belong to the *apps*; this file owns only the OSC side.
//
// Routing modes (the 3-state proxy):
//   "off"         — drop everything (pure audio passthrough in the VST form)
//   "only-control" — forward only messages whose address is in the routing map
//   "full"        — forward every received message (address as-is) to every
//                    enabled target
// ---------------------------------------------------------------------------
struct ProxyTarget {
    std::string ip;       // e.g. "127.0.0.1"
    int port = 0;         // e.g. 9001
    bool enabled = true;  // disabled targets are skipped by 'full' mode
};

// One routing rule: an incoming surface address is rewritten to
// /chain/<slot>/<param> (the Heddle set-by-name form) on target `targetIdx`.
struct ProxyRoute {
    std::string srcAddr;  // surface address, e.g. "/k/0" or "/fader/threshold"
    int targetIdx = 0;    // index into ProxyConfig::targets
    std::string slot;     // Heddle slot token on the target (id or index)
    std::string param;    // Heddle param name on the target
};

struct ProxyConfig {
    int listenPort = 9100;   // the port the surface (TouchOSC) sends to
    std::string listenHost = "127.0.0.1";
    std::string mode = "only-control";  // "off" | "only-control" | "full"
    std::vector<ProxyTarget> targets;   // up to kMaxTargets
    std::vector<ProxyRoute> routes;

    static constexpr int kMaxTargets = 5;

    // mode is valid; targets <= kMaxTargets with ip + port > 0;
    // every route references an existing target. `err` gets a message on
    // failure.
    static bool validate(const ProxyConfig& c, std::string* err);
};

// Parse a proxy config JSON object (see docs/PROXY.md for the schema).
// Same text-in/file-in split as heddle::config.
namespace proxy {
bool parse(const std::string& jsonText, ProxyConfig& out, std::string* err);
bool parseFile(const std::string& path, ProxyConfig& out, std::string* err);
// Dump a config back to JSON (stable key order, 2-space indent, round-trip).
std::string dump(const ProxyConfig& c);
}  // namespace proxy

// Result of an OSC discovery pass against one target.
struct ProxyDiscovered {
    struct Param {
        std::string slot;   // slot token = the target's slot id (chain-reply
                            // col 2), which resolveSlot() accepts AND the key
                            // /reply/<slotId>/<title> replies are filed under
        std::string param;  // param TITLE — the key findByName() matches, and
                            // the key a /chain/<slot>/<param> set needs
        int paramId = 0;    // raw VST3 param id (column 1), for reference
    };
    bool ok = false;        // target answered /reply/chain
    std::vector<Param> params;
};

}  // namespace heddle
