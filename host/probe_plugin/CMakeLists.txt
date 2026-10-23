# Raw VST3 probe plugin — a PURE Steinberg-SDK DLL (no JUCE).
#
# It exists to prove the host can drive in-block parameter curves through the
# VST3 IParamValueQueue model: process() receives a queue of (offset, value)
# points per parameter per block and linearly interpolates between them (the
# SDK contract), instead of JUCE's wrapper latching one offset-0 value per
# sub-block call.
#
# It is a separate DLL, so it compiles its own copy of the VST3 SDK base set
# (the component/controller subset). This mirrors how every real VST3 plugin
# and its DAW host each carry their own SDK objects and interoperate purely
# through COM interfaces + byte-compared GUIDs. The host links
# juce_audio_processors (its own SDK copy); loading this DLL adds a second,
# independent copy inside the DLL — no ODR clash because they live in separate
# modules and share no static state (refcounts are per-object members).
#
# NOT built with juce_add_plugin: that would pull in the whole JUCE plugin
# client + a second SDK base into the same target. A bare add_library(SHARED)
# of probe.cpp + the SDK .cpp set, with the GetPluginFactory export, is the
# minimal correct VST3 plugin.

set(VST3_SDK ${JUCE_SOURCE_DIR}/modules/juce_audio_processors/format_types/VST3_SDK)

# The component/controller SDK source set. Same family as what JUCE compiles
# into juce_audio_processors (the non-headers-only branch of
# juce_VST3Headers.h), minus the host-only objects (hostclasses,
# moduleinfoparser, pluginterfacesupport) that a component never needs.
set(PROBE_SDK_SOURCES
    ${VST3_SDK}/base/source/baseiids.cpp
    ${VST3_SDK}/base/source/fbuffer.cpp
    ${VST3_SDK}/base/source/fdebug.cpp
    ${VST3_SDK}/base/source/fobject.cpp
    ${VST3_SDK}/base/source/fstreamer.cpp
    ${VST3_SDK}/base/source/fstring.cpp
    ${VST3_SDK}/base/source/updatehandler.cpp
    ${VST3_SDK}/base/thread/source/flock.cpp
    ${VST3_SDK}/pluginterfaces/base/coreiids.cpp
    ${VST3_SDK}/pluginterfaces/base/conststringtable.cpp
    ${VST3_SDK}/pluginterfaces/base/funknown.cpp
    ${VST3_SDK}/pluginterfaces/base/ustring.cpp
    ${VST3_SDK}/public.sdk/source/common/commonstringconvert.cpp
    ${VST3_SDK}/public.sdk/source/common/memorystream.cpp
    ${VST3_SDK}/public.sdk/source/common/pluginview.cpp
    ${VST3_SDK}/public.sdk/source/vst/utility/stringconvert.cpp
    ${VST3_SDK}/public.sdk/source/vst/utility/vst2persistence.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstbus.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstcomponent.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstcomponentbase.cpp
    ${VST3_SDK}/public.sdk/source/vst/vsteditcontroller.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstparameters.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstpresetfile.cpp)

# The DLL is named HeddleProbeAudio.vst3 so the .vst3 bundle's inner module has
# the platform-suffixed name the host expects.
add_library(HeddleProbeAudio SHARED
    probe.cpp
    ${PROBE_SDK_SOURCES})

target_include_directories(HeddleProbeAudio PRIVATE ${VST3_SDK})

# The SDK sources include <windows.h> via fplatform.h; keep them quiet.
if(WIN32)
    target_compile_definitions(HeddleProbeAudio PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX)
endif()

# Post-build: lay the DLL out as a VST3 bundle so the JUCE host can discover
# it. Mirrors the smoke plugin's artefact layout:
#   <bundle>.vst3/Contents/x86_64-win/HeddleProbeAudio.vst3
set(PROBE_BUNDLE ${CMAKE_BINARY_DIR}/HeddleProbeAudio.vst3)
set(PROBE_BUNDLE_BIN ${PROBE_BUNDLE}/Contents/x86_64-win)
add_custom_command(TARGET HeddleProbeAudio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROBE_BUNDLE_BIN}
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:HeddleProbeAudio> ${PROBE_BUNDLE_BIN}/HeddleProbeAudio.vst3
    COMMENT "Packing HeddleProbeAudio.vst3 bundle")
