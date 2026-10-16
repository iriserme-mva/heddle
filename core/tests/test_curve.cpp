// Unit tests for parameter time-series: Curve eval, ParamSpec, evalParams,
// and the JSON curve schema (parse/dump/reject) in the config layer.
#include "doctest.h"

#include "heddle/chain.hpp"
#include "heddle/config.hpp"
#include "heddle/curve.hpp"

#include <cmath>

using namespace heddle;

static float near(float a, float b) { return std::abs(a - b) < 1e-6f; }

// ---- Curve::eval -----------------------------------------------------------

TEST_CASE("curve eval: empty, single, clamp and interpolate") {
    Curve empty;
    CHECK(near(empty.eval(0), 0.f));
    CHECK(near(empty.eval(1000), 0.f));  // empty -> 0

    Curve single{{42, 0.25f}};
    CHECK(near(single.eval(0), 0.25f));
    CHECK(near(single.eval(42), 0.25f));
    CHECK(near(single.eval(100000), 0.25f));  // constant

    Curve c{{0, 0.f}, {100, 1.f}};
    CHECK(near(c.eval(-10), 0.f));          // before first -> clamp to first
    CHECK(near(c.eval(0), 0.f));
    CHECK(near(c.eval(25), 0.25f));         // linear
    CHECK(near(c.eval(50), 0.5f));
    CHECK(near(c.eval(75), 0.75f));
    CHECK(near(c.eval(100), 1.f));
    CHECK(near(c.eval(500), 1.f));          // after last -> clamp to last
}

TEST_CASE("curve eval: duplicate frames fall back to that keyframe's value") {
    Curve c{{0, 0.2f}, {100, 0.5f}, {100, 0.9f}};
    CHECK(near(c.eval(100), 0.9f));  // the upper keyframe of the duplicate pair
    CHECK(near(c.eval(50), 0.35f));  // 0.2 -> 0.5 over [0, 100]
    CHECK(near(c.eval(101), 0.9f));
}

// ---- ParamSpec / evalParams ------------------------------------------------

TEST_CASE("ParamSpec: static value is constant, curve evaluates at frame") {
    ParamSpec s(0.4f);
    CHECK_FALSE(s.isCurve());
    CHECK(near(s.at(-1), 0.4f));
    CHECK(near(s.at(0), 0.4f));
    CHECK(near(s.at(12345678), 0.4f));
    CHECK(near(s.atStart(), 0.4f));

    ParamSpec c;
    c.curve.keyframes = {{0, 0.f}, {10, 1.f}};
    CHECK(c.isCurve());
    CHECK(near(c.atStart(), 0.f));
    CHECK(near(c.at(5), 0.5f));
    CHECK(near(c.at(100), 1.f));
}

TEST_CASE("evalParams mixes static and curve params at a given frame") {
    std::vector<std::pair<std::string, ParamSpec>> params;
    params.emplace_back("Static", ParamSpec(0.3f));
    ParamSpec cutoff;
    cutoff.curve.keyframes = {{0, 0.0f}, {200, 1.0f}};
    params.emplace_back("Cutoff", std::move(cutoff));

    auto at0 = evalParams(params, 0);
    REQUIRE(at0.size() == 2);
    CHECK(near(at0[0].second, 0.3f));
    CHECK(near(at0[1].second, 0.0f));

    auto at100 = evalParams(params, 100);
    CHECK(near(at100[0].second, 0.3f));  // static unchanged by time
    CHECK(near(at100[1].second, 0.5f));  // curve interpolated

    auto at999 = evalParams(params, 999);
    CHECK(near(at999[1].second, 1.0f));  // clamped after last keyframe
}

// ---- JSON schema: parse / dump / reject ------------------------------------

TEST_CASE("config parses a curve param and static+curve can mix") {
    const char* j = R"({
      "chain": [
        { "id": "eq", "plugin": "/vst/Eq.vst3",
          "params": {
            "Mix": 0.5,
            "Cutoff": { "curve": [[0, 0.2], [45000, 0.8], [90000, 0.3]] }
          } }
      ]
    })";
    ChainConfig cfg;
    std::string err;
    REQUIRE(config::parse(j, cfg, &err));
    const SlotConfig& slot = cfg.slots[0];

    // Static value untouched.
    CHECK_FALSE(slot.paramSpec("Mix")->isCurve());
    CHECK(near(slot.paramValue("Mix"), 0.5f));

    // Curve parsed with all keyframes.
    const ParamSpec* spec = slot.paramSpec("Cutoff");
    REQUIRE(spec != nullptr);
    CHECK(spec->isCurve());
    CHECK(spec->curve.size() == 3);
    CHECK(spec->curve.keyframes[0].first == 0);
    CHECK(near(spec->curve.keyframes[0].second, 0.2f));
    CHECK(spec->curve.keyframes[1].first == 45000);
    CHECK(near(spec->curve.keyframes[1].second, 0.8f));
    CHECK(spec->curve.keyframes[2].first == 90000);
    CHECK(near(spec->curve.keyframes[2].second, 0.3f));

    // paramValue resolves curves to their frame-0 value.
    CHECK(near(slot.paramValue("Cutoff"), 0.2f));
}

TEST_CASE("curve keyframe values are clamped to 0..1 at parse") {
    const char* j = R"({
      "chain": [
        { "id": "a", "plugin": "p",
          "params": { "x": { "curve": [[0, -5.0], [10, 5.0]] } } }
      ]
    })";
    ChainConfig cfg;
    std::string err;
    REQUIRE(config::parse(j, cfg, &err));
    const ParamSpec& s = cfg.slots[0].params[0].second;
    CHECK(near(s.curve.keyframes[0].second, 0.f));
    CHECK(near(s.curve.keyframes[1].second, 1.f));
}

TEST_CASE("config rejects malformed curve params") {
    auto reject = [](const std::string& json, const char* needle) {
        ChainConfig cfg;
        std::string err;
        std::string m1 = "parse of " + json + " should fail";
        CHECK_MESSAGE(!config::parse(json, cfg, &err), m1);
        std::string m2 = std::string("expected '") + needle + "' in error: " + err;
        CHECK_MESSAGE(err.find(needle) != std::string::npos, m2);
    };
    const char* pre = "{\"chain\":[{\"id\":\"a\",\"plugin\":\"p\",\"params\":{\"x\":";
    const char* post = "}}]}";

    // unsorted keyframes
    reject(std::string(pre) + R"({"curve": [[10, 0.5], [5, 0.5]]})" + post,
           "sorted");
    // negative frame
    reject(std::string(pre) + R"({"curve": [[-1, 0.5]]})" + post,
           "frame must be >= 0");
    // empty curve
    reject(std::string(pre) + R"({"curve": []})" + post, "at least one");
    // keyframe with wrong arity
    reject(std::string(pre) + R"({"curve": [[0, 0.5, 1]]})" + post,
           "keyframes must be");
    // keyframe value not a number
    reject(std::string(pre) + R"({"curve": [[0, "loud"]]})" + post,
           "keyframes must be");
    // object without a curve array
    reject(std::string(pre) + R"({"foo": 1})" + post, "number or");
    // bare string (pre-curve rejection still works)
    reject(std::string(pre) + R"("high")" + post, "number or");
}

TEST_CASE("config dump serializes curves and round-trips") {
    const char* j = R"({
      "sample_rate": 44100,
      "chain": [
        { "id": "eq", "plugin": "/vst/Eq.vst3",
          "params": {
            "Mix": 0.5,
            "Cutoff": { "curve": [[0, 0.2], [45000, 0.8]] }
          } }
      ]
    })";
    ChainConfig in, out;
    std::string err;
    REQUIRE(config::parse(j, in, &err));
    const std::string dumped = config::dump(in);
    REQUIRE(config::parse(dumped, out, &err));

    const SlotConfig& s = out.slots[0];
    // Static stays a plain number; curve keeps its keyframes exactly.
    CHECK_FALSE(s.paramSpec("Mix")->isCurve());
    CHECK(near(s.paramValue("Mix"), 0.5f));
    const ParamSpec* spec = s.paramSpec("Cutoff");
    REQUIRE(spec != nullptr);
    CHECK(spec->isCurve());
    CHECK(spec->curve.size() == 2);
    CHECK(spec->curve.keyframes[0].first == 0);
    CHECK(near(spec->curve.keyframes[0].second, 0.2f));
    CHECK(spec->curve.keyframes[1].first == 45000);
    CHECK(near(spec->curve.keyframes[1].second, 0.8f));
    // Dumped text shape: static number, curve object.
    CHECK(dumped.find(R"("Mix": 0.5)") != std::string::npos);
    CHECK(dumped.find(R"("curve")") != std::string::npos);
}

TEST_CASE("merge keeps base curve params, override static wins per param") {
    const char* baseJson = R"({
      "chain": [
        { "id": "eq", "plugin": "/vst/Eq.vst3",
          "params": {
            "Cutoff": { "curve": [[0, 0.2], [90000, 0.8]] },
            "Mix": 0.5
          } }
      ]
    })";
    const char* overJson = R"({
      "chain": [
        { "id": "eq", "plugin": "/vst/Eq.vst3",
          "params": { "Cutoff": 0.7 } }
      ]
    })";
    ChainConfig base, over, merged;
    std::string err;
    REQUIRE(config::parse(baseJson, base, &err));
    REQUIRE(config::parse(overJson, over, &err));
    merged = config::merge(base, over);
    REQUIRE(merged.slots.size() == 1);
    const SlotConfig& s = merged.slots[0];
    // Override (static 0.7) replaces the base curve.
    CHECK_FALSE(s.paramSpec("Cutoff")->isCurve());
    CHECK(near(s.paramValue("Cutoff"), 0.7f));
    // Base extras kept.
    CHECK(near(s.paramValue("Mix"), 0.5f));

    // And the reverse: override curve replaces a base static.
    merged = config::merge(over, base);
    const SlotConfig& s2 = merged.slots[0];
    CHECK(s2.paramSpec("Cutoff")->isCurve());
    CHECK(s2.paramSpec("Cutoff")->curve.size() == 2);
    CHECK(near(s2.paramValue("Cutoff"), 0.2f));  // frame-0 of the curve
    CHECK_FALSE(s2.paramSpec("Mix")->isCurve());
}
