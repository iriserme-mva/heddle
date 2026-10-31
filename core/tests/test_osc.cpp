// Targeted tests for the OSC layer: wire round-trips, address classification,
// and the param-surface -> OSC mapping.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include "heddle/osc.hpp"

#include <cmath>

using namespace heddle;
using osc_addr::ParamAction;

TEST_SUITE("osc.wire")
{

TEST_CASE("float roundtrip") {
    auto m = OscMsg::make("/chain/comp/Threshold", 0.42f);
    const std::string enc = oscEncode(m);
    OscMsg dec;
    CHECK(oscDecode(enc.data(), enc.size(), dec));
    CHECK(dec.addr == "/chain/comp/Threshold");
    CHECK(std::string(dec.typeTags.begin(), dec.typeTags.end()) == "f");
    CHECK(dec.floats.size() == 1);
    CHECK(std::abs(dec.floats[0] - 0.42f) < 1e-6f);
}

TEST_CASE("int roundtrip") {
    auto m = OscMsg::make("/chain/comp/driver", int32_t(7));
    const std::string enc = oscEncode(m);
    OscMsg dec;
    CHECK(oscDecode(enc.data(), enc.size(), dec));
    CHECK(dec.ints.size() == 1);
    CHECK(dec.ints[0] == 7);
    CHECK(std::string(dec.typeTags.begin(), dec.typeTags.end()) == "i");
}

TEST_CASE("string roundtrip with padding and slashes") {
    auto m = OscMsg::make("/chain/apply", std::string("C:/VSTs/My Plugin.vst3"));
    const std::string enc = oscEncode(m);
    OscMsg dec;
    CHECK(oscDecode(enc.data(), enc.size(), dec));
    CHECK(dec.strs.size() == 1);
    CHECK(dec.strs[0] == "C:/VSTs/My Plugin.vst3");
}

TEST_CASE("blob roundtrip with binary content") {
    const std::string payload = "{\"chain\":[{\"id\":\"a\",\"plugin\":\"x.vst3\"}]}";
    auto m = OscMsg::make("/chain/config", payload.data(), payload.size());
    const std::string enc = oscEncode(m);
    OscMsg dec;
    CHECK(oscDecode(enc.data(), enc.size(), dec));
    CHECK(std::string(dec.typeTags.begin(), dec.typeTags.end()) == "b");
    CHECK(dec.strs.size() == 1);
    CHECK(dec.strs[0] == payload);
}

TEST_CASE("multi-arg roundtrip") {
    OscMsg m;
    m.addr = "/set";
    m.typeTags = {'i', 'f', 's'};
    m.ints.push_back(3);
    m.floats.push_back(-1.5f);
    m.strs.push_back("hello");
    const std::string enc = oscEncode(m);
    OscMsg dec;
    CHECK(oscDecode(enc.data(), enc.size(), dec));
    CHECK(dec.ints[0] == 3);
    CHECK(std::abs(dec.floats[0] - (-1.5f)) < 1e-6f);
    CHECK(dec.strs[0] == "hello");
}

TEST_CASE("rejects truncated and empty input") {
    OscMsg dec;
    CHECK_FALSE(oscDecode(nullptr, 0, dec));
    const std::string enc = oscEncode(OscMsg::make("/x", 1.0f));
    CHECK_FALSE(oscDecode(enc.data(), 4, dec));                 // only header
    CHECK_FALSE(oscDecode(enc.data(), enc.size() - 1, dec));    // truncated body
    const char garbage[8] = {0, 0, 0, 1, 0, 0, 0, 0};
    CHECK_FALSE(oscDecode(garbage, 8, dec));
}

TEST_CASE("rejects args that run past the declared frame length") {
    // A malformed (or malicious) encoder may understate the length header:
    // declare only addr+tags while the tag section still demands a float.
    // The decoder must refuse the frame — not read the "argument" out of
    // whatever bytes follow the declared frame in the datagram buffer.
    const std::string good = oscEncode(OscMsg::make("/x", 1.0f));
    REQUIRE(good.size() == 4 + 12);  // hdr + addr(4) + tags(4) + float(4)
    std::string frame = good;
    const uint32_t bogus = 8;  // addr(4) + tags(4): stops before the float
    frame[0] = char((bogus >> 24) & 0xff);
    frame[1] = char((bogus >> 16) & 0xff);
    frame[2] = char((bogus >> 8) & 0xff);
    frame[3] = char(bogus & 0xff);
    for (size_t i = 12; i < frame.size(); ++i) frame[i] = char(0xAB);  // sentinel
    OscMsg dec;
    CHECK_FALSE(oscDecode(frame.data(), frame.size(), dec));
}

TEST_CASE("ignores trailing bytes beyond the declared frame length") {
    // The declared length, not the buffer size, delimits a message: padding
    // or a coalesced following datagram after 4+total is not part of it.
    const std::string enc = oscEncode(OscMsg::make("/x", 1.0f));
    std::string padded = enc;
    padded += "\xAA\xAA\xAA\xAA";
    OscMsg dec;
    CHECK(oscDecode(padded.data(), padded.size(), dec));
    CHECK(dec.addr == "/x");
    CHECK(dec.floats.size() == 1);
}

TEST_CASE("rejects tag args that underflow the frame") {
    OscMsg dec;
    // Tags claim a string but the frame ends before the NUL terminator.
    // addr "/x\0\0" (4) + tags ",s\0\0" (4) = 8 declared, no string bytes.
    const char buf[12] = {0, 0, 0, 8, '/','x',0,0, ',', 's',0,0};
    CHECK_FALSE(oscDecode(buf, sizeof(buf), dec));
    // Tags claim a blob whose length field itself lies past the frame end.
    const char buf2[12] = {0, 0, 0, 8, '/','x',0,0, ',', 'b',0,0};
    CHECK_FALSE(oscDecode(buf2, sizeof(buf2), dec));
    // pos>limit guard: declared total is NOT 4-aligned (11). addr(4) +
    // tags ",sf"(8) + an empty string at offset 12 whose NUL pads to 16 —
    // past the declared end (15). The trailing 'f' tag would then demand a
    // 4-byte float: with a bare `k <= limit - pos` the comparison underflows
    // size_t to true and the read runs 4 bytes off the end of the datagram.
    // The decoder must reject the frame instead.
    const char buf3[15] = {0, 0, 0, 11, '/','x',0,0, ',', 's','f',0, 0, 0, 0};
    CHECK_FALSE(oscDecode(buf3, sizeof(buf3), dec));
}

TEST_CASE("accepts bare OSC 1.0 (no length header) — the TouchOSC/Live/Max form") {
    // External control surfaces do NOT send the Heddle length-prefixed frame.
    // They emit a plain OSC 1.0 datagram: address, tags, args, no 4-byte
    // length header. This is the form TouchOSC emits for a fader at /f1.
    // Before the dual-framing fix, oscDecode read the first four address
    // bytes (/f1\0 = 0x2f663100 ~ 793M) as the length, saw it exceed the
    // packet, and dropped the message silently — so no external surface
    // could ever reach the proxy. These are the regression guards.
    //
    // addr "/f1\0" (4, already aligned) + tags ",f\0\0" (4) + float(4) = 12B.
    std::string bare;
    bare += "/f1";      bare += '\0';
    bare += ",f";       bare += '\0'; bare += '\0';
    float v = 0.5f; uint32_t u; std::memcpy(&u, &v, 4);
    bare += char((u >> 24) & 0xff); bare += char((u >> 16) & 0xff);
    bare += char((u >> 8) & 0xff);  bare += char(u & 0xff);
    OscMsg dec;
    CHECK(oscDecode(bare.data(), bare.size(), dec));
    CHECK(dec.addr == "/f1");
    CHECK(std::string(dec.typeTags.begin(), dec.typeTags.end()) == "f");
    CHECK(dec.floats.size() == 1);
    CHECK(std::abs(dec.floats[0] - 0.5f) < 1e-6f);
}

TEST_CASE("bare OSC with no args decodes as a query") {
    // A bare fader query /f1 with no args: addr "/f1\0" (4) + tags ",\0\0\0" (4).
    const char bare[8] = {'/','f','1',0, ',', 0, 0, 0};
    OscMsg dec;
    CHECK(oscDecode(bare, sizeof(bare), dec));
    CHECK(dec.addr == "/f1");
    CHECK(dec.typeTags.empty());
}

TEST_CASE("bare OSC matches its length-prefixed twin") {
    // The same logical message must decode identically whether or not the
    // 4-byte length header is present — internal (host<->proxy) traffic and
    // external-surface traffic must be indistinguishable to the rest of the
    // pipeline. Build both by hand for addr "/chain/comp/Threshold", f 0.6.
    //   body: addr 24B ("/chain/comp/Threshold\0" + 3 pad) + tags ",f\0\0" 4B
    //         + float 4B = 32B; prefixed = 4-byte header (0x20) + body.
    std::string body;
    body += "/chain/comp/Threshold"; body += '\0';
    body.append(24u - body.size(), '\0');      // pad addr field to 24
    body += ",f"; body += '\0'; body += '\0';  // tags
    float v = 0.6f; uint32_t u; std::memcpy(&u, &v, 4);
    body += char((u >> 24) & 0xff); body += char((u >> 16) & 0xff);
    body += char((u >> 8) & 0xff);  body += char(u & 0xff);
    REQUIRE(body.size() == 32);
    std::string prefixed;
    uint32_t total = (uint32_t) body.size();
    prefixed += char((total >> 24) & 0xff); prefixed += char((total >> 16) & 0xff);
    prefixed += char((total >> 8) & 0xff);  prefixed += char(total & 0xff);
    prefixed += body;
    OscMsg a, b;
    CHECK(oscDecode(body.data(), body.size(), a));
    CHECK(oscDecode(prefixed.data(), prefixed.size(), b));
    CHECK(a.addr == b.addr);
    CHECK(a.addr == "/chain/comp/Threshold");
    CHECK(std::string(a.typeTags.begin(), a.typeTags.end()) == "f");
    CHECK(std::abs(a.floats[0] - 0.6f) < 1e-6f);
    CHECK(std::abs(a.floats[0] - b.floats[0]) < 1e-6f);
}

TEST_CASE("bare OSC with a multi-arg int+float set decodes") {
    // set by id: addr "/chain/0/Cutoff\0" (16B) + tags ",if\0" (4B) + int(4)
    // + float(4) = 28B, no length header.
    std::string bare;
    bare += "/chain/0/Cutoff"; bare += '\0';
    bare += ",if"; bare += '\0';
    uint32_t iu = 17, fu = 0; float f = 0.25f; std::memcpy(&fu, &f, 4);
    for (uint32_t w : {iu, fu})
        for (int s = 24; s >= 0; s -= 8) bare += char((w >> s) & 0xff);
    OscMsg dec;
    CHECK(oscDecode(bare.data(), bare.size(), dec));
    CHECK(dec.addr == "/chain/0/Cutoff");
    CHECK(dec.ints.size() == 1);
    CHECK(dec.ints[0] == 17);
    CHECK(dec.floats.size() == 1);
    CHECK(std::abs(dec.floats[0] - 0.25f) < 1e-6f);
}
}  // namespace
TEST_SUITE_END();

TEST_SUITE("osc.addr")
{

TEST_CASE("classifies chain queries") {
    CHECK(osc_addr::classify(OscMsg::make("/chain/query", std::string(""))).op ==
          ParamAction::Op::QueryChain);
    CHECK(osc_addr::classify(OscMsg::make("/chain/reset")).op ==
          ParamAction::Op::ResetAll);
}

TEST_CASE("classifies set by name (float)") {
    auto a = osc_addr::classify(OscMsg::make("/chain/comp/Threshold", 0.5f));
    CHECK(a.op == ParamAction::Op::SetByName);
    CHECK(a.slotToken == "comp");
    CHECK(a.param == "Threshold");
    CHECK(std::abs(a.value - 0.5f) < 1e-6f);
}

TEST_CASE("classifies set by id (int + float)") {
    OscMsg m;
    m.addr = "/chain/0/Cutoff";
    m.typeTags = {'i', 'f'};
    m.ints.push_back(17);
    m.floats.push_back(0.25f);
    auto a = osc_addr::classify(m);
    CHECK(a.op == ParamAction::Op::SetById);
    CHECK(a.slotToken == "0");
    CHECK(a.paramId == 17);
    CHECK(std::abs(a.value - 0.25f) < 1e-6f);
}

TEST_CASE("classifies reset slot") {
    auto a = osc_addr::classify(OscMsg::make("/chain/fx2/reset"));
    CHECK(a.op == ParamAction::Op::ResetSlot);
    CHECK(a.slotToken == "fx2");
}

TEST_CASE("classifies bare /chain/<slot> as a slot param query") {
    // The proxy's DISCOVER issues a bare /chain/<slot> (no args) after
    // /chain/query to enumerate a slot's params — it must classify as
    // QueryAll with the slot token (regression: it used to be Unknown).
    auto a = osc_addr::classify(OscMsg::make("/chain/smoke"));
    CHECK(a.op == ParamAction::Op::QueryAll);
    CHECK(a.slotToken == "smoke");
    CHECK(a.param.empty());
    auto b = osc_addr::classify(OscMsg::make("/chain/0"));
    CHECK(b.op == ParamAction::Op::QueryAll);
    CHECK(b.slotToken == "0");
}

TEST_CASE("classifies reserved /chain/quit and /chain/status as Unknown") {
    // Reserved host-control words (host_main.cpp extension branch) must NOT
    // be swallowed by the bare /chain/<slot> -> QueryAll mapping, or the host
    // can no longer intercept /chain/quit (-> "quitting" + clean exit) or
    // /chain/status (-> status line). Regression: the QueryAll mapping once
    // swept these into handleQuerySlot -> "param not found", so the host
    // never quit (CI bridge-test "no 'quitting' for /chain/quit").
    auto q = osc_addr::classify(OscMsg::make("/chain/quit"));
    CHECK(q.op == ParamAction::Op::Unknown);
    CHECK(!q.ok());
    auto s = osc_addr::classify(OscMsg::make("/chain/status"));
    CHECK(s.op == ParamAction::Op::Unknown);
    CHECK(!s.ok());
}

TEST_CASE("classifies apply path and config blob") {
    auto p = osc_addr::classify(OscMsg::make("/chain/apply", std::string("/tmp/c.json")));
    CHECK(p.op == ParamAction::Op::ApplyPath);
    CHECK(p.blob == "/tmp/c.json");
    const std::string json = "{\"chain\":[]}";
    auto b = osc_addr::classify(OscMsg::make("/chain/config", json.data(), json.size()));
    CHECK(b.op == ParamAction::Op::ApplyBlob);
    CHECK(b.blob == json);
}

TEST_CASE("rejects unknown prefixes") {
    CHECK_FALSE(osc_addr::classify(OscMsg::make("/foo/bar", 1.0f)).ok());
    CHECK_FALSE(osc_addr::classify(OscMsg::make("/chain", 1.0f)).ok());
}

TEST_CASE("paramSetToOsc emits one message per param with metadata") {
    ParamSet ps;
    ps.pluginId = "comp";
    ps.params.push_back(Param{0, "Threshold", "dB", 0.6f, 0.6f, -24.f, 0.f, 0,
                              ParamKind::Float, true, {}});
    ps.params.push_back(Param{1, "Mode", "", 0.f, 0.f, 0.f, 1.f, 4,
                              ParamKind::Enum, true, {"A", "B", "C", "D"}});
    auto msgs = osc_addr::paramSetToOsc(ps);
    CHECK(msgs.size() == 2);
    CHECK(msgs[0].addr == "/reply/comp/Threshold");
    CHECK(msgs[1].addr == "/reply/comp/Mode");
    // line: id<TAB>title<TAB>value<TAB>min<TAB>max<TAB>steps<TAB>kind[<TAB>labels]
    CHECK(msgs[1].strs[0].find("1\tMode") == 0);
    CHECK(msgs[1].strs[0].find("4\tenum") != std::string::npos);
    CHECK(msgs[1].strs[0].find("A\tB\tC\tD") != std::string::npos);
}

}  // namespace
TEST_SUITE_END();
