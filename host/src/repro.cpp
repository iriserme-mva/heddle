// heddle_repro — reproduces the Ableton "open a plugin" sequence against a
// real .vst3 and logs each phase to stdout (flushed). The LAST phase logged
// is the freeze point when the process is killed by an external timeout.
//
//   usage: heddle_repro <path-to-plugin.vst3> [pumpSeconds]
//
// Phases (each flushed before the next begins):
//   init -> format -> types -> instantiate(+VST3 initialize) -> prepare ->
//   setStateInformation(re-apply own state, exercises engine start) ->
//   createEditor -> addToDesktop -> pump(N s) -> clean

#include <chrono>
#include <cstdio>
#include <string>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

static double nowMs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
               .count() / 1000.0;
}
static void step(const char* s) {
    std::printf("[%10.3f ms] %s\n", nowMs(), s);
    std::fflush(stdout);
}
static void pump(double seconds, const char* label) {
    step(label);
    const auto end = std::chrono::steady_clock::now()
                   + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    std::printf("   %s done\n", label);
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-plugin.vst3> [pumpSeconds]\n",
                     argv[0]);
        return 0;
    }
    const std::string path = argv[1];
    const double pumpSec = (argc >= 3) ? std::atof(argv[2]) : 8.0;

    step("init JUCE GUI");
    juce::initialiseJuce_GUI();

    juce::File file(juce::String(path.c_str()));
    if (!file.exists()) { step("FILE NOT FOUND"); return 2; }

    juce::AudioPluginFormatManager mgr;
    mgr.addDefaultFormats();
    juce::AudioPluginFormat* format = nullptr;
    for (int i = 0; i < mgr.getNumFormats(); ++i) {
        auto* f = mgr.getFormat(i);
        if (f != nullptr &&
            f->fileMightContainThisPluginType(file.getFullPathName())) {
            format = f;
            break;
        }
    }
    if (format == nullptr) { step("NO FORMAT CAN LOAD"); return 2; }
    step("format ok");

    juce::OwnedArray<juce::PluginDescription> descs;
    format->findAllTypesForFile(descs, file.getFullPathName());
    if (descs.isEmpty()) { step("NO TYPES"); return 2; }
    step("types ok");

    const juce::PluginDescription& desc = *descs[0];
    juce::String errStr;
    auto instance = mgr.createPluginInstance(desc, 44100.0, 512, errStr);
    if (instance == nullptr) {
        std::printf("INstantiate FAILED: %s\n", errStr.toRawUTF8());
        std::fflush(stdout);
        return 3;
    }
    step("instantiate ok (VST3 initialize done internally)");

    instance->prepareToPlay(44100.0, 512);
    step("prepareToPlay ok");

    // In a DAW the message loop runs continuously from instantiation, so by
    // the time the user opens the editor, every processor Timer has been
    // ticking for seconds. Replicate that: pump before the editor so the
    // lazy engine-start (first timer tick) happens now, not mid-open.
    pump(2.0, "pump-before-editor");

    // Re-apply the plugin's own (default) state. This drives
    // setStateInformation() -> (proxy) engine start, the second open-path
    // candidate. A freshly-instantiated instance's state is its defaults, so
    // this is a safe, bounded exercise.
    juce::MemoryBlock state;
    instance->getStateInformation(state);
    if (!state.isEmpty()) {
        instance->setStateInformation(state.getData(), state.getSize());
        step("setStateInformation ok (own state re-applied)");
    } else {
        step("getStateInformation empty (skipping state re-apply)");
    }

    juce::AudioProcessorEditor* ed = instance->createEditor();
    step("createEditor returned");
    if (ed != nullptr) {
        ed->setSize(900, 700);
        ed->addToDesktop(0);
        step("addToDesktop ok");
    } else {
        step("createEditor returned NULL (no GUI editor)");
    }

    const auto end = std::chrono::steady_clock::now()
                   + std::chrono::duration<double>(pumpSec);
    int ticks = 0;
    while (std::chrono::steady_clock::now() < end) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
        if (++ticks % 40 == 0) {
            std::printf("   pump alive tick %d\n", ticks);
            std::fflush(stdout);
        }
    }
    step("pump window complete (no hard hang)");

    instance->releaseResources();
    std::printf("REPRO_EXIT_CLEAN\n");
    std::fflush(stdout);
    return 0;
}
