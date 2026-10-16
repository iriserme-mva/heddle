// Targeted tests for config JSON: schema validation, round-trip, merge.
#include "doctest.h"

#include "heddle/config.hpp"

#include <fstream>
#include <filesystem>

using namespace heddle;

static const char* kGood = R"({
  "sample_rate": 48000,
  "block_size": 1024,
  "chain": [
    {
      "id": "comp",
      "plugin": "/opt/vst/Comp.vst3",
      "params": { "Threshold": 0.6, "Ratio": 0.3, "Enable": 1.0 }
    },
    {
      "id": "dly",
      "plugin": "/opt/vst/Dly.vst3",
      "enabled": false
    }
  ]
})";

TEST_CASE("parses a valid config") {
    ChainConfig cfg;
    std::string err;
    REQUIRE(config::parse(kGood, cfg, &err));
    CHECK(cfg.sampleRate == 48000);
    CHECK(cfg.blockSize == 1024);
    CHECK(cfg.slots.size() == 2);
    CHECK(cfg.slots[0].id == "comp");
    CHECK(cfg.slots[0].pluginPath == "/opt/vst/Comp.vst3");
    CHECK(cfg.slots[0].hasParam("Threshold"));
    CHECK(std::abs(cfg.slots[0].paramValue("Threshold") - 0.6f) < 1e-6f);
    CHECK(cfg.slots[0].paramValue("Missing", 0.75f) == 0.75f);
    CHECK_FALSE(cfg.slots[1].enabled);
}

TEST_CASE("clamps out-of-range param values") {
    const char* j = R"({"chain":[{"id":"a","plugin":"p","params":{"x":-3.0,"y":2.0}}]})";
    ChainConfig cfg;
    std::string err;
    REQUIRE(config::parse(j, cfg, &err));
    CHECK(cfg.slots[0].paramValue("x") == 0.f);
    CHECK(cfg.slots[0].paramValue("y") == 1.f);
}

TEST_CASE("rejects malformed configs") {
    ChainConfig cfg;
    std::string err;
    CHECK_FALSE(config::parse("not json", cfg, &err));
    CHECK_FALSE(err.empty());

    // empty chain is valid
    CHECK(config::parse(R"({"chain":[]})", cfg, &err));
    CHECK(cfg.slots.empty());
    // missing chain
    CHECK_FALSE(config::parse(R"({"sample_rate":44100})", cfg, &err));
    // missing id
    CHECK_FALSE(config::parse(R"({"chain":[{"plugin":"p"}]})", cfg, &err));
    // missing plugin
    CHECK_FALSE(config::parse(R"({"chain":[{"id":"a"}]})", cfg, &err));
    // duplicate ids
    CHECK_FALSE(config::parse(
        R"({"chain":[{"id":"a","plugin":"p"},{"id":"a","plugin":"q"}]})", cfg, &err));
    // non-number param
    CHECK_FALSE(config::parse(
        R"({"chain":[{"id":"a","plugin":"p","params":{"x":"high"}}]})", cfg, &err));
}

TEST_CASE("dump round-trips a parsed config") {
    ChainConfig in, out;
    std::string err;
    REQUIRE(config::parse(kGood, in, &err));
    const std::string dumped = config::dump(in);
    REQUIRE(config::parse(dumped, out, &err));
    CHECK(out.sampleRate == in.sampleRate);
    CHECK(out.blockSize == in.blockSize);
    CHECK(out.slots.size() == in.slots.size());
    for (size_t i = 0; i < in.slots.size(); ++i) {
        CHECK(out.slots[i].id == in.slots[i].id);
        CHECK(out.slots[i].pluginPath == in.slots[i].pluginPath);
        CHECK(out.slots[i].enabled == in.slots[i].enabled);
        CHECK(out.slots[i].params.size() == in.slots[i].params.size());
    }
}

TEST_CASE("merge: override wins per-slot and per-param, base extras kept") {
    ChainConfig base, over, merged;
    std::string err;
    REQUIRE(config::parse(kGood, base, &err));
    const char* overJson = R"({
      "sample_rate": 96000,
      "chain": [
        { "id": "comp", "plugin": "/opt/vst/Comp.vst3", "params": { "Ratio": 0.9 } },
        { "id": "sat", "plugin": "/opt/vst/Sat.vst3", "params": { "Drive": 0.5 } }
      ]
    })";
    REQUIRE(config::parse(overJson, over, &err));
    merged = config::merge(base, over);
    CHECK(merged.sampleRate == 96000);
    CHECK(merged.slots.size() == 3);  // comp, dly (base), sat (new)
    // comp: ratio overridden, threshold kept from base
    const SlotConfig* comp = nullptr;
    for (const auto& s : merged.slots)
        if (s.id == "comp") comp = &s;
    REQUIRE(comp);
    CHECK(std::abs(comp->paramValue("Ratio") - 0.9f) < 1e-6f);
    CHECK(std::abs(comp->paramValue("Threshold") - 0.6f) < 1e-6f);
    // dly from base still disabled
    bool dlyDisabled = false;
    for (const auto& s : merged.slots)
        if (s.id == "dly") dlyDisabled = !s.enabled;
    CHECK(dlyDisabled);
}

TEST_CASE("parses osc section (port + bind_host)") {
    ChainConfig cfg;
    std::string err;
    REQUIRE(config::parse(
        R"({"osc": {"port": 9000, "bind_host": "0.0.0.0"}, "chain": []})",
        cfg, &err));
    CHECK(cfg.osc.port == 9000);
    CHECK(cfg.osc.bindHost == "0.0.0.0");

    // missing osc -> disabled defaults
    REQUIRE(config::parse(R"({"chain": []})", cfg, &err));
    CHECK(cfg.osc.port == 0);
    CHECK(cfg.osc.bindHost == "127.0.0.1");
}

TEST_CASE("rejects bad osc port values") {
    ChainConfig cfg;
    std::string err;
    CHECK_FALSE(config::parse(R"({"osc": {"port": -1}, "chain": []})", cfg, &err));
    CHECK_FALSE(config::parse(R"({"osc": {"port": 70000}, "chain": []})", cfg, &err));
    CHECK_FALSE(config::parse(R"({"osc": {"port": "9000"}, "chain": []})", cfg, &err));
}

TEST_CASE("dump round-trips osc settings") {
    ChainConfig in, out;
    std::string err;
    REQUIRE(config::parse(R"({"osc": {"port": 9000}, "chain": []})", in, &err));
    const std::string dumped = config::dump(in);
    REQUIRE(config::parse(dumped, out, &err));
    CHECK(out.osc.port == 9000);
    CHECK(out.osc.bindHost == in.osc.bindHost);

    // port 0 + default host -> no osc key emitted, still parses back
    ChainConfig in2, out2;
    REQUIRE(config::parse(R"({"chain": []})", in2, &err));
    const std::string dumped2 = config::dump(in2);
    REQUIRE(config::parse(dumped2, out2, &err));
    CHECK(out2.osc.port == 0);
    CHECK(dumped2.find("\"osc\"") == std::string::npos);
}

TEST_CASE("merge: override osc port wins, base kept when override silent") {
    ChainConfig base, over, merged;
    std::string err;
    REQUIRE(config::parse(
        R"({"osc": {"port": 9000, "bind_host": "0.0.0.0"}, "chain": []})", base, &err));

    // override specifies a port -> wins; host stays from base
    REQUIRE(config::parse(R"({"osc": {"port": 9001}, "chain": []})", over, &err));
    merged = config::merge(base, over);
    CHECK(merged.osc.port == 9001);
    CHECK(merged.osc.bindHost == "0.0.0.0");

    // override silent on osc -> base kept
    REQUIRE(config::parse(R"({"chain": []})", over, &err));
    merged = config::merge(base, over);
    CHECK(merged.osc.port == 9000);
    CHECK(merged.osc.bindHost == "0.0.0.0");
}

TEST_CASE("parseFile reads from disk") {
    const std::string path =
        (std::filesystem::temp_directory_path() / "heddle_test_config.json").string();
    {
        std::ofstream f(path, std::ios::binary);
        f << kGood;
    }
    ChainConfig cfg;
    std::string err;
    REQUIRE(config::parseFile(path, cfg, &err));
    CHECK(cfg.slots.size() == 2);
    const std::string missing =
        (std::filesystem::temp_directory_path() / "heddle_does_not_exist_xyz.json").string();
    ChainConfig bad;
    CHECK_FALSE(config::parseFile(missing, bad, &err));
    std::remove(path.c_str());
}
