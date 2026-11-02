#pragma once
#include <cmath>
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
    exp,       // y = x^K      (fades / gain trims: slow out; K=1 == x^2)
    invexp,    // y = 1-(1-x)^K (snappy start, slow out; K=1 == the old 2nd power)
    scurve,    // S-shape around 0.5 (see curvature; 0=linear, 1=smoothstep)
};

// Shape a [0,1] value with the given curve. `curvature` is the single per-
// route steepness knob, gamma-style:
//   exp/invexp : y = x^K / 1-(1-x)^K, K clamped to [0.1, 10].
//                K = 1 reproduces the historical x^2 / 1-(1-x)^2 exactly
//                (backward compatible default); K < 1 snaps to the start,
//                K > 1 softens the output.
//   scurve     : 0 = straight line, 1 = the classic smoothstep, (1, 2]
//                re-applies the S so the mid gets steeper (K = 2 ==
//                smoothstep-of-smoothstep); clamped to [0, 2].
// identity/linear ignore it. Every case is monotonic and maps [0,1] -> [0,1].
inline float proxyCurveEval(ProxyCurve c, float x, double curvature = 1.0) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    switch (c) {
        case ProxyCurve::exp:
        case ProxyCurve::invexp: {
            // Gamma-style: the curve is the identity power map raised to K.
            // K = 1 is the old 2nd-power behaviour, so stored configs and
            // old UI states keep their shape untouched.
            const float k = (float) curvature;
            const float k01 = (k < 0.1f) ? 0.1f : (k > 10.0f ? 10.0f : k);
            const float t = (c == ProxyCurve::exp) ? x : (1.0f - x);
            const float y = std::powf(t, 2.0f * k01);
            return (c == ProxyCurve::exp) ? y : (1.0f - y);
        }
        case ProxyCurve::scurve: {
            const float k = (float) curvature;
            const float k02 = (k < 0.0f) ? 0.0f : (k > 2.0f ? 2.0f : k);
            // Pass 1: blend line -> smoothstep (weight clamped to [0,1] so
            // the convex combination stays monotonic and in-range).
            const float w1 = (k02 > 1.0f) ? 1.0f : k02;
            const auto sstep = [](float v) { return v * v * (3.0f - 2.0f * v); };
            float y = (1.0f - w1) * x + w1 * sstep(x);
            // Pass 2 (k > 1): re-shape for a steeper mid; composition of
            // monotonic endpoint-fixing maps stays monotonic and in-range.
            if (k02 > 1.0f)
                y = (1.0f - (k02 - 1.0f)) * y + (k02 - 1.0f) * sstep(y);
            return y;
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
    // Curve steepness knob K (gamma-style). exp/invexp: y = x^K / 1-(1-x)^K
    // (K = 1 is the historical 2nd power). scurve: 0 = straight line,
    // 1 = classic smoothstep (default), (1, 2] = steeper mid. Ignored by
    // identity/linear. See proxyCurveEval.
    double curvature = 1.0;
};

struct ProxyConfig {
    int listenPort = 9100;   // the port the surface (TouchOSC) sends to
    // Default: all interfaces. A surface (TouchOSC on a phone) is usually on
    // the LAN, not on loopback — binding 127.0.0.1 made the proxy invisible
    // to any remote surface (only host->proxy traffic on the same machine
    // worked). Set "127.0.0.1" explicitly for loopback-only setups.
    std::string listenHost = "0.0.0.0";
    std::string mode = "only-control";  // "off" | "only-control" | "full"
    std::vector<ProxyTarget> targets;   // unbounded (a normal rig has hundreds)
    std::vector<ProxyRoute> routes;

    // mode is valid; every target has ip + port > 0;
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
