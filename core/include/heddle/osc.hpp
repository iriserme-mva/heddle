#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "heddle/param.hpp"

namespace heddle {

// Minimal OSC 1.0 message model (tag-based, enough for Heddle's protocol).
// Types: 'f' float32, 'i' int32, 's' string, 'b' blob.
struct OscMsg {
    std::string addr;
    std::vector<float> floats;
    std::vector<int32_t> ints;
    std::vector<std::string> strs;   // includes blob payloads as strings
    std::vector<char> typeTags;      // in arg order: 'f','i','s','b'

    static OscMsg make(const std::string& addr) {
        OscMsg m; m.addr = addr; return m;
    }
    static OscMsg make(const std::string& addr, float v) {
        OscMsg m; m.addr = addr; m.floats.push_back(v); m.typeTags.push_back('f'); return m;
    }
    // N-float form: [value, (delay), (duration)] for sub-block scheduling.
    static OscMsg make(const std::string& addr, const std::vector<float>& fs) {
        OscMsg m; m.addr = addr;
        for (float f : fs) { m.floats.push_back(f); m.typeTags.push_back('f'); }
        return m;
    }
    static OscMsg make(const std::string& addr, int32_t v) {
        OscMsg m; m.addr = addr; m.ints.push_back(v); m.typeTags.push_back('i'); return m;
    }
    static OscMsg make(const std::string& addr, const std::string& v) {
        OscMsg m; m.addr = addr; m.strs.push_back(v); m.typeTags.push_back('s'); return m;
    }
    static OscMsg make(const std::string& addr, const char* blob, size_t len) {
        OscMsg m; m.addr = addr; m.strs.emplace_back(blob, len); m.typeTags.push_back('b'); return m;
    }
    bool empty() const { return typeTags.empty(); }
};

// OSC 1.0 wire serialization (4-byte length header + addr + tags + args).
std::string oscEncode(const OscMsg& msg);
// Returns false on malformed input.
bool oscDecode(const char* data, size_t len, OscMsg& out);

// ---------------------------------------------------------------------------
// Addressing scheme (see docs/OSC.md)
//
//   /chain/query                            list slots           -> /reply/chain
//   /chain/<slot>/<param>        (float)    set param by name
//   /chain/<slot>/<param>        (int id)   set param by id
//   /chain/<slot>/reset                        reset slot params
//   /chain/reset                               reset all slots
//   /chain/apply             (string path)    (re)load config file
//   /chain/config                       (blob)     (re)load config from JSON blob
//
// Replies / pushes:
//   /reply/chain                          slot list (string)
//   /reply/<slot>                         all params of slot, one string line each:
//                                         "id<TAB>title<TAB>normalized<TAB>min<TAB>max<TAB>steps<TAB>kind"
//   /reply/<slot>/<param>                 ack of a set: current normalized value
//   /change/<slot>/<param>                push: plugin changed its own param (float norm)
//   /status                               (string) host status
// ---------------------------------------------------------------------------
namespace osc_addr {

// Parse "/chain/..." into a ParamAction. Unknown prefixes -> op==Unknown.
struct ParamAction {
    enum class Op {
        SetByName, SetById, ResetSlot, ResetAll, ApplyPath, ApplyBlob,
        QueryAll, QueryChain, Unknown
    } op = Op::Unknown;
    std::string slotToken;   // slot id (string) or numeric index ("0"); resolve via Chain
    std::string param;       // param name (SetByName)
    uint32_t paramId = 0;    // param id (SetById)
    float value = 0.f;       // normalized value (SetBy*)
    // Sub-block timing (whole-queue raw path). delay = hold time before the
    // ramp begins, in BLOCKS (0 = start at the earliest renderable block);
    // duration = ramp length in BLOCKS (0 = hard snap = 1-sample jump).
    // Wire contract: 1 float [value] -> default ramp (host's kLookahead),
    // 2 floats [value,delay] -> delayed default ramp, 3 floats
    // [value,delay,duration] -> explicit. duration==-1.f = "use the host's
    // default ramp length" sentinel (kLookahead is a host constant, core
    // cannot know it) -- the DEFAULT, so a programmatic set without explicit
    // timing behaves exactly like the legacy single-float wire set.
    float delay = 0.f;
    float duration = -1.f;   // 0 = hard snap; >0 = ramp blocks; -1 = default
    std::string blob;        // path or config json (Apply*)

    bool ok() const { return op != Op::Unknown; }
};

ParamAction classify(const OscMsg& msg);

// Build "/reply/<slot>" param-list messages for a ParamSet (one OscMsg per param).
std::vector<OscMsg> paramSetToOsc(const ParamSet& ps);

std::string makeChangeAddr(int slot, const std::string& paramName);
std::string makeReplySlotAddr(int slot);

}  // namespace osc_addr

}  // namespace heddle
