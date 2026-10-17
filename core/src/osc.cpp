#include "heddle/osc.hpp"

#include <cstring>

namespace heddle {

namespace {

size_t pad4(size_t n) { return (n + 3) & ~size_t(3); }

void putU32(std::string& s, uint32_t v) {
    s.push_back(char((v >> 24) & 0xff));
    s.push_back(char((v >> 16) & 0xff));
    s.push_back(char((v >> 8) & 0xff));
    s.push_back(char(v & 0xff));
}
void putF32(std::string& s, float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    putU32(s, u);
}

struct Reader {
    const char* p;
    size_t limit;  // end of the declared frame: 4 + declared-length header
    size_t pos = 0;

    // Every read must stay inside the *declared* frame, not merely the
    // buffer the datagram happened to occupy: bytes past the declared
    // length are not part of this message, so a parse that would reach
    // them is malformed. Guarded against pos>limit (a malformed frame whose
    // final-string padding overshoots the declared end) — a bare
    // `k <= limit - pos` would underflow size_t and wrongly return true,
    // letting a later arg read run off the end of the datagram.
    bool have(size_t k) const { return pos <= limit && k <= limit - pos; }
    char peek() const { return have(1) ? p[pos] : 0; }
    void skip(size_t k) { pos += k; }

    uint32_t u32() {
        if (!have(4)) return 0;
        uint32_t v = (uint32_t(uint8_t(p[pos])) << 24) |
                     (uint32_t(uint8_t(p[pos + 1])) << 16) |
                     (uint32_t(uint8_t(p[pos + 2])) << 8) |
                     uint32_t(uint8_t(p[pos + 3]));
        pos += 4;
        return v;
    }
    float f32() {
        uint32_t u = u32();
        float v;
        std::memcpy(&v, &u, 4);
        return v;
    }
    // OSC strings are NUL-padded to 4-byte boundary (NUL included).
    // Returns {value, ok}; ok=false when the string is unterminated.
    std::pair<std::string, bool> oscString() {
        size_t start = pos;
        while (have(1) && p[pos] != 0) pos++;
        if (!have(1)) return {std::string(), false};  // unterminated
        std::string out(p + start, pos - start);
        pos++;  // consume NUL
        pos = pad4(pos);
        return {out, true};
    }
};

}  // namespace

std::string oscEncode(const OscMsg& msg) {
    std::string s;
    // addr (OSC string)
    s += msg.addr;
    s.push_back('\0');
    s.append(pad4(s.size()) - s.size(), '\0');
    // type tags
    std::string tags = ",";
    for (char t : msg.typeTags) tags.push_back(t);
    tags.push_back('\0');
    s.append(tags);
    s.append(pad4(s.size()) - s.size(), '\0');
    // args
    size_t fi = 0, ii = 0, si = 0;
    for (char t : msg.typeTags) {
        switch (t) {
        case 'f': putF32(s, msg.floats[fi++]); break;
        case 'i': putU32(s, uint32_t(msg.ints[ii++])); break;
        case 's': {
            s += msg.strs[si];
            s.push_back('\0');
            s.append(pad4(s.size()) - s.size(), '\0');
            si++;
            break;
        }
        case 'b': {
            const auto& b = msg.strs[si++];
            putU32(s, uint32_t(b.size()));
            s += b;
            s.append(pad4(s.size()) - s.size(), '\0');
            break;
        }
        default: break;
        }
    }
    // 4-byte total length header
    std::string body;
    putU32(body, uint32_t(s.size()));
    body += s;
    return body;
}

bool oscDecode(const char* data, size_t len, OscMsg& out) {
    if (!data || len < 4 + 1) return false;
    Reader r{data, len};
    uint32_t total = r.u32();
    if (total == 0 || total > len - 4) return false;
    // Parse only the declared frame. Bytes after 4+total belong to the
    // next datagram (or padding), and a malformed tag section must not be
    // able to read them: bound every read to the declared length.
    r.limit = 4 + size_t(total);
    out = OscMsg{};
    auto addrPair = r.oscString();
    out.addr = addrPair.first;
    if (!addrPair.second || !r.have(1) || r.peek() != ',') return false;
    r.skip(1);  // ','
    auto tagsPair = r.oscString();
    if (!tagsPair.second) return false;
    const std::string& tags = tagsPair.first;
    out.typeTags.assign(tags.begin(), tags.end());
    size_t fi = 0, ii = 0, si = 0;
    for (char t : tags) {
        switch (t) {
        case 'f':
            if (!r.have(4)) return false;
            out.floats.push_back(r.f32());
            break;
        case 'i':
            if (!r.have(4)) return false;
            out.ints.push_back(int32_t(r.u32()));
            break;
        case 's': {
            auto sp = r.oscString();
            if (!sp.second) return false;
            out.strs.push_back(sp.first);
            break;
        }
        case 'b': {
            if (!r.have(4)) return false;
            uint32_t blen = r.u32();
            if (!r.have(blen)) return false;
            out.strs.emplace_back(r.p + r.pos, blen);
            r.skip(blen);
            r.pos = pad4(r.pos);
            break;
        }
        default: return false;  // unknown type tag
        }
    }
    (void)fi; (void)ii; (void)si;
    return true;
}

namespace osc_addr {

namespace {
bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}
std::string token(const std::string& addr, size_t& i) {
    size_t start = i;
    while (i < addr.size() && addr[i] != '/') i++;
    return addr.substr(start, i - start);
}
}  // namespace

ParamAction classify(const OscMsg& msg) {
    ParamAction a;
    const std::string& addr = msg.addr;
    if (addr == "/chain/query") { a.op = ParamAction::Op::QueryChain; return a; }
    if (addr == "/chain/reset") { a.op = ParamAction::Op::ResetAll; return a; }
    if (addr == "/chain/apply") {
        a.op = ParamAction::Op::ApplyPath;
        a.blob = msg.strs.empty() ? "" : msg.strs[0];
        return a;
    }
    if (addr == "/chain/config") {
        a.op = ParamAction::Op::ApplyBlob;
        a.blob = msg.strs.empty() ? "" : msg.strs[0];
        return a;
    }
    if (addr.rfind("/chain/", 0) != 0) return a;
    std::string rest = addr.substr(7);  // after "/chain/"
    size_t i = 0;
    std::string slotTok = token(rest, i);
    if (slotTok.empty()) return a;
    a.slotToken = slotTok;
    if (rest.size() == slotTok.size()) return a;  // no more parts
    if (rest[i] != '/') return a;
    i++;
    std::string second = token(rest, i);
    if (second.empty()) return a;

    if (second == "reset") { a.op = ParamAction::Op::ResetSlot; return a; }

    // /chain/<slot>/<param>
    a.param = second;
    if (msg.typeTags.empty()) { a.op = ParamAction::Op::QueryAll; return a; }
    if (msg.typeTags[0] == 'f') { a.op = ParamAction::Op::SetByName; a.value = msg.floats[0]; }
    else if (msg.typeTags[0] == 'i') {
        a.op = ParamAction::Op::SetById;
        a.paramId = uint32_t(msg.ints[0]);
        if (msg.typeTags.size() >= 2 && msg.typeTags[1] == 'f') a.value = msg.floats[0];
        else a.value = Param::clamp01(float(msg.ints[0]) / 1000.f);
    } else return a;
    return a;
}

std::vector<OscMsg> paramSetToOsc(const ParamSet& ps) {
    std::vector<OscMsg> out;
    out.reserve(ps.params.size());
    const std::string base = "/reply/" + ps.pluginId;
    for (const auto& p : ps.params) {
        // Tab-separated: id, title, value(norm), min, max, steps, kind
        std::string line = std::to_string(p.id) + "\t" + p.title + "\t" +
                           std::to_string(p.value) + "\t" + std::to_string(p.minValue) +
                           "\t" + std::to_string(p.maxValue) + "\t" +
                           std::to_string(p.stepCount) + "\t" +
                           (p.kind == ParamKind::Enum ? "enum" :
                            p.kind == ParamKind::Bool ? "bool" :
                            p.kind == ParamKind::Chord ? "chord" : "float");
        for (const auto& li : p.listItems) line += "\t" + li;  // optional labels
        out.push_back(OscMsg::make(base + "/" + p.title, line));
    }
    return out;
}

std::string makeChangeAddr(int slot, const std::string& paramName) {
    return "/change/" + std::to_string(slot) + "/" + paramName;
}
std::string makeReplySlotAddr(int slot) {
    return "/reply/" + std::to_string(slot);
}

}  // namespace osc_addr
}  // namespace heddle
