#include "weft/chain.hpp"

#include <cstring>
#include <cstdlib>

namespace weft {

namespace {
std::string normalizePath(const std::string& p) {
    std::string s = p;
    for (auto& c : s)
        if (c == '\\') c = '/';
    return s;
}
}  // namespace

bool Chain::load(const ChainConfig& cfg, std::string* err) {
    sets_.clear();
    sets_.reserve(cfg.slots.size());
    for (const auto& slot : cfg.slots) {
        ParamSet ps;
        if (!slot.enabled) {
            // Still represent the slot (OSC addressing stays stable) but with
            // no plugin loaded.
            ps.pluginId = slot.id;
            ps.pluginName = "(disabled)";
            sets_.push_back(std::move(ps));
            continue;
        }
        if (!backend_.load(slot, ps)) {
            if (err) *err = "failed to load plugin for slot '" + slot.id +
                            "': " + slot.pluginPath;
            sets_.clear();
            return false;
        }
        ps.pluginId = slot.id;
        ps.loadedPath = normalizePath(slot.pluginPath);
        // Apply configured params on top of plugin defaults. Curve params
        // resolve to their frame-0 (timeline start) value at load time; the
        // renderer re-evaluates them per block as the timeline advances.
        for (const auto& kv : slot.params) {
            if (auto* p = ps.findByName(kv.first))
                p->value = Param::clamp01(kv.second.atStart());
        }
        // Unknown names are kept in config (may appear after hot-swap);
        // they are not applied now.
        // Model updates alone are invisible to the audio — the values must be
        // pushed into the live plugin instance. (This is what makes a config's
        // `params:` block actually change what the chain renders.)
        backend_.setParams(slot.id, evalParams(slot.params, 0));
        sets_.push_back(std::move(ps));
    }
    cfg_ = cfg;
    return true;
}

bool Chain::apply(const ChainConfig& cfg, std::string* err) {
    // For each configured slot: load if not loaded from the same path, then
    // (re)apply its params. Loaded slots whose path unchanged are untouched.
    for (const auto& slot : cfg.slots) {
        const std::string path = normalizePath(slot.pluginPath);
        bool found = false;
        for (auto& ps : sets_) {
            if (ps.pluginId != slot.id) continue;
            found = true;
            if (!slot.enabled) continue;
            if (ps.loadedPath != path || ps.params.empty() && !ps.pluginName.empty() &&
                ps.pluginName == "(disabled)") {
                // path changed (or was disabled) -> reload
                ParamSet fresh;
                if (!backend_.load(slot, fresh)) {
                    if (err) *err = "hot-apply: failed to load '" + slot.pluginPath + "'";
                    return false;
                }
                fresh.pluginId = slot.id;
                fresh.loadedPath = path;
                ps = std::move(fresh);
            }
            for (const auto& kv : slot.params)
                if (auto* p = ps.findByName(kv.first)) p->value = Param::clamp01(kv.second.atStart());
            backend_.setParams(slot.id, evalParams(slot.params, 0));
            break;
        }
        if (!found) {
            ParamSet ps;
            if (slot.enabled && !backend_.load(slot, ps)) {
                if (err) *err = "hot-apply: failed to load '" + slot.pluginPath + "'";
                return false;
            }
            ps.pluginId = slot.id;
            ps.loadedPath = path;
            ps.pluginName = slot.enabled ? (ps.pluginName.empty() ? slot.id : ps.pluginName)
                                         : std::string("(disabled)");
            if (!slot.enabled) ps.pluginName = "(disabled)";
            for (const auto& kv : slot.params)
                if (auto* p = ps.findByName(kv.first)) p->value = Param::clamp01(kv.second.atStart());
            sets_.push_back(std::move(ps));
        }
    }
    cfg_ = cfg;
    return true;
}

void Chain::resetToDefaults() {
    for (auto& ps : sets_)
        for (auto& p : ps.params) p.value = p.defaultValue;
}

int Chain::resolveSlot(const std::string& token) const {
    if (token.empty()) return -1;
    bool numeric = true;
    for (char c : token)
        if (c < '0' || c > '9') { numeric = false; break; }
    if (numeric) {
        int idx = std::atoi(token.c_str());
        if (idx >= 0 && idx < (int)sets_.size()) return idx;
        return -1;
    }
    for (size_t i = 0; i < sets_.size(); ++i)
        if (sets_[i].pluginId == token) return int(i);
    return -1;
}

float Chain::setParam(const std::string& slotToken, const std::string& name, float norm) {
    const int idx = resolveSlot(slotToken);
    if (idx < 0) return -1.f;
    const auto* p = sets_[idx].findByName(name);
    if (!p) return -1.f;
    return setParamById(slotToken, p->id, norm);
}

float Chain::setParamById(const std::string& slotToken, uint32_t id, float norm) {
    const int idx = resolveSlot(slotToken);
    if (idx < 0) return -1.f;
    auto* p = sets_[idx].find(id);
    if (!p) return -1.f;
    const float v = Param::clamp01(norm);
    if (!backend_.setParam(sets_[idx].pluginId, id, v)) return -1.f;
    p->value = v;
    return p->value;
}

}  // namespace weft
