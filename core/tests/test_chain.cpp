// Targeted tests for the Chain: mock backend, load / hot-apply / reset /
// slot resolution / change propagation.
#include "doctest.h"

#include "heddle/chain.hpp"

#include <cmath>
#include <map>
#include <set>

using namespace heddle;

// Mock backend: a fixed param surface per plugin path, records setParam calls.
class MockBackend : public IPluginBackend {
public:
    struct Surface {
        std::vector<Param> params;
    };
    void addSurface(const std::string& path, std::vector<Param> params) {
        surfaces_[path] = Surface{std::move(params)};
    }

    bool load(const SlotConfig& slot, ParamSet& out) override {
        auto it = surfaces_.find(slot.pluginPath);
        if (it == surfaces_.end()) return false;
        loadCount_[slot.id]++;
        out.params = it->second.params;
        return true;
    }
    bool setParam(const std::string& slotId, uint32_t id, float norm) override {
        (void)slotId;
        for (auto& [path, s] : surfaces_)
            for (auto& p : s.params)
                if (p.id == id) { sets_[id] = norm; return true; }
        return false;
    }
    bool setParams(const std::string& slotId,
                   const std::vector<std::pair<std::string, float>>& namedValues) override {
        // Record the (slot, name) -> value pushes so tests can verify the
        // config's `params:` block actually reached the backend, not just the
        // cached model. Name-keyed to avoid cross-slot id collisions.
        for (const auto& kv : namedValues)
            namedSets_[slotId + "::" + kv.first] = Param::clamp01(kv.second);
        return true;
    }

    size_t loads(const std::string& slotId) const {
        auto it = loadCount_.find(slotId);
        return it == loadCount_.end() ? 0 : it->second;
    }
    float lastSet(uint32_t id) const {
        auto it = sets_.find(id);
        return it == sets_.end() ? -1.f : it->second;
    }
    float lastSetNamed(const std::string& slotId, const std::string& name) const {
        auto it = namedSets_.find(slotId + "::" + name);
        return it == namedSets_.end() ? -1.f : it->second;
    }

private:
    std::map<std::string, Surface> surfaces_;
    std::map<std::string, int> loadCount_;
    std::map<uint32_t, float> sets_;
    std::map<std::string, float> namedSets_;
};

static Param thr(std::string units, float dv) {
    return Param{0, "Threshold", std::move(units), dv, dv, -24.f, 0.f, 0, ParamKind::Float, true, {}};
}
static Param ratio() {
    return Param{1, "Ratio", "", 0.3f, 0.3f, 0.f, 1.f, 0, ParamKind::Float, true, {}};
}
static Param timeParam(std::string units, float dv) {
    return Param{0, "Time", std::move(units), dv, dv, 0.f, 500.f, 0, ParamKind::Float, true, {}};
}

static ChainConfig makeCfg() {
    ChainConfig c;
    c.sampleRate = 44100;
    c.blockSize = 512;
    SlotConfig comp;
    comp.id = "comp";
    comp.pluginPath = "/vst/Comp.vst3";
    comp.params = {{"Threshold", 0.6f}};
    SlotConfig dly;
    dly.id = "dly";
    dly.pluginPath = "/vst/Dly.vst3";
    c.slots = {comp, dly};
    return c;
}

TEST_CASE("load pushes configured params to the plugin instance") {
    // Regression: the config's `params:` block must reach the live plugin,
    // not just the cached ParamSet model. If load() only updates the model,
    // an offline render ignores the config and this fails.
    MockBackend be;
    be.addSurface("/vst/Comp.vst3", {thr("dB", 0.5f), ratio()});
    be.addSurface("/vst/Dly.vst3", {timeParam("ms", 0.2f)});
    Chain chain(be);
    std::string err;
    REQUIRE(chain.load(makeCfg(), &err));
    // comp is configured with Threshold=0.6 — the backend must have received
    // the push for it (not just the model update).
    CHECK(std::abs(be.lastSetNamed("comp", "Threshold") - 0.6f) < 1e-6f);
}

TEST_CASE("load applies configured params on top of plugin defaults") {
    MockBackend be;
    be.addSurface("/vst/Comp.vst3", {thr("dB", 0.5f), ratio()});
    be.addSurface("/vst/Dly.vst3", {timeParam("ms", 0.2f)});
    Chain chain(be);
    std::string err;
    REQUIRE(chain.load(makeCfg(), &err));
    CHECK(chain.numSlots() == 2);
    const ParamSet* ps = chain.slot(0);
    REQUIRE(ps);
    const Param* thrp = ps->findByName("Threshold");
    REQUIRE(thrp);
    CHECK(std::abs(thrp->value - 0.6f) < 1e-6f);  // configured, not the 0.5 default
    const Param* ratio = ps->findByName("Ratio");
    CHECK(std::abs(ratio->value - 0.3f) < 1e-6f);  // untouched default
    CHECK(chain.slotName(1) == "dly");
}

TEST_CASE("load fails cleanly on unknown plugin") {
    MockBackend be;  // no surfaces
    Chain chain(be);
    std::string err;
    CHECK_FALSE(chain.load(makeCfg(), &err));
    CHECK(err.find("comp") != std::string::npos);
}

TEST_CASE("setParam by name and by id update surface and backend") {
    MockBackend be;
    be.addSurface("/vst/Comp.vst3", {thr("dB", 0.5f), ratio()});
    be.addSurface("/vst/Dly.vst3", {timeParam("ms", 0.2f)});
    Chain chain(be);
    REQUIRE(chain.load(makeCfg(), nullptr));

    CHECK(std::abs(chain.setParam("comp", "Ratio", 0.9f) - 0.9f) < 1e-6f);
    CHECK(std::abs(be.lastSet(1) - 0.9f) < 1e-6f);  // backend saw it
    CHECK(std::abs(chain.setParamById("0", 0, 0.1f) - 0.1f) < 1e-6f);  // index token
    CHECK(std::abs(chain.slot(0)->findByName("Ratio")->value - 0.9f) < 1e-6f);

    CHECK(chain.setParam("nope", "Ratio", 0.5f) < 0.f);   // unknown slot
    CHECK(chain.setParam("comp", "Nope", 0.5f) < 0.f);    // unknown param
    CHECK(chain.setParam("dly", "Ratio", 0.5f) < 0.f);    // param not on this slot
}

TEST_CASE("resolveSlot accepts index and id") {
    MockBackend be;
    be.addSurface("/vst/Comp.vst3", {Param{0, "Threshold", "", 0.f, 0.f, 0.f, 1.f, 0,
                                           ParamKind::Float, true, {}}});
    be.addSurface("/vst/Dly.vst3", {Param{0, "Time", "", 0.f, 0.f, 0.f, 1.f, 0,
                                          ParamKind::Float, true, {}}});
    Chain chain(be);
    REQUIRE(chain.load(makeCfg(), nullptr));
    CHECK(chain.resolveSlot("0") == 0);
    CHECK(chain.resolveSlot("1") == 1);
    CHECK(chain.resolveSlot("comp") == 0);
    CHECK(chain.resolveSlot("dly") == 1);
    CHECK(chain.resolveSlot("9") == -1);
    CHECK(chain.resolveSlot("missing") == -1);
}

TEST_CASE("resetToDefaults restores plugin defaults") {
    MockBackend be;
    be.addSurface("/vst/Comp.vst3", {thr("dB", 0.5f), ratio()});
    be.addSurface("/vst/Dly.vst3", {timeParam("ms", 0.2f)});
    Chain chain(be);
    REQUIRE(chain.load(makeCfg(), nullptr));
    chain.setParam("comp", "Ratio", 0.9f);
    chain.resetToDefaults();
    CHECK(std::abs(chain.slot(0)->findByName("Ratio")->value - 0.3f) < 1e-6f);
    CHECK(std::abs(chain.slot(0)->findByName("Threshold")->value - 0.5f) < 1e-6f);
}

TEST_CASE("hot apply reloads only when path changes, keeps loaded otherwise") {
    MockBackend be;
    be.addSurface("/vst/Comp.vst3", {Param{0, "Threshold", "dB", 0.5f, 0.5f, -24.f, 0.f, 0,
                                           ParamKind::Float, true, {}}});
    be.addSurface("/vst/Comp2.vst3", {Param{0, "Threshold", "dB", 0.4f, 0.4f, -24.f, 0.f, 0,
                                            ParamKind::Float, true, {}}});
    be.addSurface("/vst/Dly.vst3", {timeParam("ms", 0.2f)});
    be.addSurface("/vst/Sat.vst3", {Param{0, "Drive", "", 0.1f, 0.1f, 0.f, 1.f, 0,
                                          ParamKind::Float, true, {}}});
    Chain chain(be);
    REQUIRE(chain.load(makeCfg(), nullptr));
    CHECK(be.loads("comp") == 1);
    CHECK(be.loads("dly") == 1);

    // Same paths -> no reloads, params re-applied.
    auto cfg2 = makeCfg();
    cfg2.slots[0].params = {{"Threshold", 0.8f}};
    REQUIRE(chain.apply(cfg2, nullptr));
    CHECK(be.loads("comp") == 1);
    CHECK(be.loads("dly") == 1);
    CHECK(std::abs(chain.slot(0)->findByName("Threshold")->value - 0.8f) < 1e-6f);

    // Path change -> exactly one reload of that slot; new slot added.
    auto cfg3 = makeCfg();
    cfg3.slots[0].pluginPath = "/vst/Comp2.vst3";
    SlotConfig sat;
    sat.id = "sat";
    sat.pluginPath = "/vst/Sat.vst3";
    sat.params = {{"Drive", 0.7f}};
    cfg3.slots.push_back(sat);
    REQUIRE(chain.apply(cfg3, nullptr));
    CHECK(be.loads("comp") == 2);   // reloaded
    CHECK(be.loads("dly") == 1);    // untouched
    CHECK(be.loads("sat") == 1);    // new
    CHECK(chain.numSlots() == 3);
    // Comp2 surface default is 0.4, but makeCfg() re-applies Threshold=0.6.
    CHECK(std::abs(chain.slot(0)->findByName("Threshold")->value - 0.6f) < 1e-6f);
    CHECK(std::abs(chain.setParam("sat", "Drive", 0.7f) - 0.7f) < 1e-6f);
}

TEST_CASE("plugin-originated changes propagate to subscribers") {
    MockBackend be;
    be.addSurface("/vst/Comp.vst3", {Param{0, "Threshold", "dB", 0.5f, 0.5f, -24.f, 0.f, 0,
                                           ParamKind::Float, true, {}}});
    be.addSurface("/vst/Dly.vst3", {timeParam("ms", 0.2f)});
    Chain chain(be);
    REQUIRE(chain.load(makeCfg(), nullptr));

    std::vector<std::pair<int, std::string>> changes;
    chain.onChange([&](int slot, const Param& p) { changes.emplace_back(slot, p.title); });

    chain.onPluginParamChanged("comp", 0, 0.77f);
    CHECK(changes.size() == 1);
    CHECK(changes[0].first == 0);
    CHECK(changes[0].second == "Threshold");
    CHECK(std::abs(chain.slot(0)->findByName("Threshold")->value - 0.77f) < 1e-6f);

    // Same value again -> no duplicate notification.
    chain.onPluginParamChanged("comp", 0, 0.77f);
    CHECK(changes.size() == 1);

    // Unknown param id -> ignored.
    chain.onPluginParamChanged("dly", 99, 0.5f);
    CHECK(changes.size() == 1);
}
