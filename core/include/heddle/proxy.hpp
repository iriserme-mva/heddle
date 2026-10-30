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

// ---------------------------------------------------------------------------
// Per-route value mapping (input-to-output).
//
// A surface control's raw float is (1) picked by index (`ProxyRoute::arg` —
// X/Y knobs send two values; index 1 would otherwise be read as the optional
// "delay" by the target), (2) rescaled from [inMin, inMax] to [0,1] and
// clamped, then (3) shaped by a transfer curve. The identity/0..1 defaults
// reproduce the old "forward verbatim" behaviour for a single-arg float, so
// existing configs and plain sliders need no changes.
// ---------------------------------------------------------------------------
enum class ProxyCurve {
    identity,  // y = x (default)
    linear,    // == identity (alias kept for explicit configs)
    exp,       // y = x^2      (fades / gain trims: slow out)
    invexp,    // y = 1-(1-x)^2 (snappy start, slow out)
    scurve,    // S-shape around 0.5 (sharper mid)
};

inline float proxyCurveEval(ProxyCurve c, float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    switch (c) {
        case ProxyCurve::exp:
            return x * x;
        case ProxyCurve::invexp: {
            const float i = 1.0f - x;
            return 1.0f - i * i;
        }
        case ProxyCurve::scurve: {
            // 3x^2 - 2x^3 (smoothstep): monotonic, 0->0, 1->1, kink-free.
            return x * x * (3.0f - 2.0f * x);
        }
        case ProxyCurve::identity:
        case ProxyCurve::linear:
        default:
            return x;
    }
}

inline bool proxyCurveFromName(const std::string& n, ProxyCurve* out) {
    if (n == "identity") { *out = ProxyCurve::identity; return true; }
    if (n == "linear")   { *out = ProxyCurve::linear;   return true; }
    if (n == "exp")      { *out = ProxyCurve::exp;      return true; }
    if (n == "invexp")   { *out = ProxyCurve::invexp;   return true; }
    if (n == "scurve")   { *out = ProxyCurve::scurve;   return true; }
    return false;
}

inline const char* proxyCurveName(ProxyCurve c) {
    switch (c) {
        case ProxyCurve::linear:   return "linear";
        case ProxyCurve::exp:      return "exp";
        case ProxyCurve::invexp:   return "invexp";
        case ProxyCurve::scurve:   return "scurve";
        case ProxyCurve::identity:
        default:                   return "identity";
    }
}

// One routing rule: an incoming surface address is rewritten to
// /chain/<slot>/<param> (the Heddle set-by-name form) on target `targetIdx`.
struct ProxyRoute {
    std::string srcAddr;  // surface address, e.g. "/k/0" or "/fader/threshold"
    int targetIdx = 0;    // index into ProxyConfig::targets
    std::string slot;     // Heddle slot token on the target (id or index)
    std::string param;    // Heddle param name on the target
    // Which float argument of the incoming message carries the value
    // (0 = first, 1 = second, ...). Messages with fewer floats are dropped
    // for this route (see ProxyEngine::pump).
    int arg = 0;
    // Input range: the picked float is remapped from [inMin, inMax] to [0,1]
    // (clamped) before the curve. Defaults (0..1) keep the value as-is.
    // inMin > inMax INVERTS the mapping (for a reversed surface control,
    // e.g. a flipped slider or a TouchOSC control with swapped range).
    double inMin = 0.0;
    double inMax = 1.0;
    ProxyCurve curve = ProxyCurve::identity;
};

struct ProxyConfig {
    int listenPort = 9100;   // the port the surface (TouchOSC) sends to
    // Default: all interfaces. A surface (TouchOSC on a phone) is usually on
    // the LAN, not on loopback — binding 127.0.0.1 made the proxy invisible
    // to any remote surface (only host->proxy traffic on the same machine
    // worked). Set "127.0.0.1" explicitly for loopback-only setups.
    std::string listenHost = "0.0.0.0";
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
