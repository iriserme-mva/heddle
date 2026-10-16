// heddle_host_smoke — loads a VST3 plugin and dumps its parameter surface
// as JSON to stdout. No args → print usage, exit 0.

#include <cstdio>
#include <string>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>  // juce::initialiseJuce_GUI()

#include "JuceBackend.hpp"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace {

json toJson(const heddle::Param& p) {
    json j;
    j["id"] = p.id;
    j["title"] = p.title;
    j["units"] = p.units;
    j["defaultValue"] = p.defaultValue;
    j["value"] = p.value;
    j["minValue"] = p.minValue;
    j["maxValue"] = p.maxValue;
    j["stepCount"] = p.stepCount;
    j["automatable"] = p.automatable;
    j["listItems"] = p.listItems;
    return j;
}

json toJson(const heddle::ParamSet& set) {
    json j;
    j["slotId"] = set.pluginId;
    j["pluginName"] = set.pluginName;
    j["loadedPath"] = set.loadedPath;
    json arr = json::array();
    for (const auto& p : set.params)
        arr.push_back(toJson(p));
    j["params"] = arr;
    return j;
}

}  // namespace

int main(int argc, char** argv) {
    juce::initialiseJuce_GUI();

    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <path-to-plugin.vst3>\n"
                     "       Loads the plugin and prints its parameter surface\n"
                     "       as JSON to stdout.\n",
                     argv[0]);
        return 0;
    }

    const std::string path = argv[1];
    heddle::JuceBackend backend;
    heddle::ParamSet set;
    std::string err;
    if (!backend.loadSlot("smoke", path, set, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    std::printf("%s\n", toJson(set).dump(2).c_str());
    return 0;
}
