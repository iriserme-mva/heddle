# WeftBridge VST3 -- the DAW-facing half of the t5 bridge.
#
# Routes DAW audio to/from a running weft-host over named shared-memory rings
# (weft::AudioRing, from weft_core). Built in CI as a compile + instantiation
# check (the weft-bridge-test round-trip drives the rings directly; it does
# not load this plugin).
#
# NOTE: juce_add_modules() is called by JUCE's top-level CMakeLists (via
# FetchContent at the host level) -- do NOT call it again here.

juce_add_plugin(weft_bridge_plugin
    FORMATS VST3
    PRODUCT_NAME "WeftBridge"
    NAME "WeftBridge"
    MANUFACTURER "Weft"
    COMPANY_NAME "Weft"
    COMPANY_WEBSITE "https://github.com/iriserme-mva/weft"
    COMPANY_COPYRIGHT "Copyright (c) 2026 Weft"
    BUNDLE_ID "com.weft.bridge"
    PLUGIN_CODE "WftB"
    VERSION "1.0.0"
    # Same crash as the smoke plugin (juce_vst3_helper 0xC0000005/SIGSEGV on
    # CI with JUCE 8.0.4). WeftBridge is a manual/DAW plugin, so moduleinfo
    # scanning is not required.
    VST3_AUTO_MANIFEST FALSE)

target_sources(weft_bridge_plugin PRIVATE bridge.cpp)
target_link_libraries(weft_bridge_plugin
    PRIVATE
        weft_core
        juce::juce_audio_processors
        juce::juce_graphics
        juce::juce_gui_basics)

# The status editor needs the GUI stack (juce_gui_basics). The core AudioRing
# API needs only weft_core. No audio devices and -- critically -- no
# juce_audio_plugin_client (the plugin target already compiles the VST3 SDK
# base; a second copy would ODR-conflict. Same rule as every host target).

# Headless CI: disable the web browser so the juce_gui_extra sources
# (compiled into this target via the gui modules) don't drag in GTK (Linux)
# or WebKit (macOS).
target_compile_definitions(weft_bridge_plugin PRIVATE JUCE_WEB_BROWSER=0)

# No networking: JUCE_USE_CURL defaults to 1 and #includes <curl/curl.h> on
# Linux/BSD, where CI runners lack libcurl dev headers.
target_compile_definitions(weft_bridge_plugin PRIVATE JUCE_USE_CURL=0)

# Must come AFTER juce_add_plugin: it writes into the target's generated
# sources dir. Produces JuceHeader.h (module includes + JucePlugin_CreateInstance).
juce_generate_juce_header(weft_bridge_plugin)
