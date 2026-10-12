// Targeted tests for the OSC layer: wire round-trips, address classification,
// and the param-surface -> OSC mapping.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include "weft/osc.hpp"

#include <cmath>

using namespace weft;
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
