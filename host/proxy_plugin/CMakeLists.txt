# heddle-proxy passthrough VST3 (routes one OSC surface to up to 5 Heddle
# targets; audio passes through unchanged). Reuses heddle_core's ProxyEngine
# (same code as the standalone heddle-proxy exe). See docs/PROXY.md.
#
# The router runs on a juce::Timer on the message thread — the audio callback
# never touches the engine, so this plugin is a pure passthrough DSP-wise.
#
# NOTE: juce_add_modules() is called by JUCE's top-level CMakeLists (via
# FetchContent at the host level) — do NOT call it again here.

juce_add_plugin(heddle_proxy_plugin
    FORMATS VST3
    PRODUCT_NAME "HeddleProxy"
    NAME "HeddleProxy"
    MANUFACTURER "Heddle"
    COMPANY_NAME "Heddle"
    COMPANY_WEBSITE "https://github.com/iriserme-mva/heddle"
    COMPANY_COPYRIGHT "Copyright (c) 2026 Heddle"
    BUNDLE_ID "com.heddle.proxy"
    PLUGIN_CODE "HdPr"
    VERSION "1.0.0"
    # VST3_AUTO_MANIFEST FALSE: juce_vst3_helper crashes on the CI images
    # (JUCE 8.0.4); the smoke host discovers VST3s via direct factory
    # enumeration when moduleinfo.json is absent. Matches test_plugin.
    VST3_AUTO_MANIFEST FALSE)

target_sources(heddle_proxy_plugin PRIVATE plugin.cpp)
target_link_libraries(heddle_proxy_plugin PRIVATE
    juce::juce_audio_processors
    juce::juce_gui_basics
    heddle_core)
# Editor uses juce_gui_basics components (Button/Label/TextEditor/ListBox).
target_compile_definitions(heddle_proxy_plugin PRIVATE JUCE_WEB_BROWSER=0)
target_compile_definitions(heddle_proxy_plugin PRIVATE JUCE_USE_CURL=0)
if(MSVC)
    # plugin.cpp contains UTF-8 literals (em dashes) — compile as UTF-8 so
    # wide-string conversion does not mangle them.
    target_compile_options(heddle_proxy_plugin PRIVATE /utf-8)
endif()

juce_generate_juce_header(heddle_proxy_plugin)
