cmake_minimum_required(VERSION 3.16)
project(heddle_core VERSION 0.1.14 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- core library -----------------------------------------------------------
# POSITION_INDEPENDENT_CODE: the static lib is also linked into shared
# objects on Linux (VST3 .so in heddle_bridge); without -fPIC the .so link
# fails with "relocation R_X86_64_PC32 ... recompile with -fPIC".
add_library(heddle_core STATIC
    core/src/osc.cpp
    core/src/config.cpp
    core/src/chain.cpp
    core/src/curve.cpp
    core/src/osc_io.cpp
    core/src/ring.cpp
    core/src/proxy.cpp
)
set_target_properties(heddle_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(heddle_core PUBLIC
  core/include
  third_party
)
if(WIN32)
  target_compile_definitions(heddle_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
  # osc_io.cpp uses Winsock on Windows; propagate the import library to every
  # consumer of the static lib (tests, hosts, probes).
  target_link_libraries(heddle_core PUBLIC ws2_32)
endif()

# ---- tests ------------------------------------------------------------------
enable_testing()
add_executable(heddle_core_tests
  core/tests/test_osc.cpp
  core/tests/test_config.cpp
  core/tests/test_chain.cpp
  core/tests/test_curve.cpp
  core/tests/test_osc_udp.cpp
  core/tests/test_ring.cpp
  core/tests/test_proxy.cpp
)
target_link_libraries(heddle_core_tests PRIVATE heddle_core)
target_include_directories(heddle_core_tests PRIVATE third_party/doctest)
add_test(NAME heddle_core_tests COMMAND heddle_core_tests)

# ---- latency probe (t4) -----------------------------------------------------
# ring_latency: instrumented added-latency benchmark for the shared-memory
# ring (in-process bench + cross-process real-time paced measurement).
add_executable(heddle_ring_latency core/tools/ring_latency.cpp)
target_link_libraries(heddle_ring_latency PRIVATE heddle_core)

# ---- OSC performance benchmark (core layer, no JUCE) ------------------------
# osc_perf: OSC codec throughput, per-op cost of the classify->resolveSlot->
# setParam path (continuous param adjust), UDP loopback one-way + set->ack
# RTT, and max sustainable set rate. Runs on every CI runner in the `core`
# job (no audio device / X server needed). See docs/PERFORMANCE.md.
add_executable(heddle_osc_perf core/tools/osc_perf.cpp)
target_link_libraries(heddle_osc_perf PRIVATE heddle_core)

# PE version resource: one shared .rc.in, configured per-target with its own
# InternalName/OriginalFilename (configure_file per target, since the two
# values differ). Windows "Details" tab shows Heddle version instead of
# "Version 1.0.0.0". Only MSVC/Windows builds compile it in (WIN32).
if(WIN32)
  set(HEDDLE_VRC_VERSION "${PROJECT_VERSION}")
  set(HEDDLE_VRC_MAJOR   "${PROJECT_VERSION_MAJOR}")
  set(HEDDLE_VRC_MINOR   "${PROJECT_VERSION_MINOR}")
  set(HEDDLE_VRC_PATCH   "${PROJECT_VERSION_PATCH}")
  function(heddle_stamp_version target)
    set(HEDDLE_INTERNAL_NAME ${target})
    # OUTPUT_NAME is only set explicitly on heddle-proxy (value == target name);
    # the other targets default to their target name. Use get_target_property but
    # guard against the "OUTPUT_NAME-NOTFOUND" literal CMake returns for an unset
    # property, falling back to the target name in both cases.
    get_target_property(_hvn ${target} OUTPUT_NAME)
    if(NOT _hvn OR _hvn STREQUAL "OUTPUT_NAME-NOTFOUND")
      set(_hvn ${target})
    endif()
    set(HEDDLE_OUTPUT_NAME ${_hvn})
    configure_file(${CMAKE_SOURCE_DIR}/version.rc.in
                   ${CMAKE_CURRENT_BINARY_DIR}/${target}.version.rc @ONLY)
    target_sources(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${target}.version.rc)
  endfunction()
endif()

# ---- heddle-proxy (standalone OSC fan-out router) ---------------------------
# Core-layer (JUCE-free): routes one OSC surface to any number of Heddle targets.
# The passthrough VST3 form lives in host/ and reuses the same ProxyEngine.
# See docs/PROXY.md.
add_executable(heddle-proxy core/tools/proxy_main.cpp)
target_link_libraries(heddle-proxy PRIVATE heddle_core)
set_target_properties(heddle-proxy PROPERTIES OUTPUT_NAME heddle-proxy)
if(WIN32)
  heddle_stamp_version(heddle-proxy)
endif()

# ---- heddle-osc-send (test connector) --------------------------------------
# Emulates a control surface (TouchOSC etc.): sends bare OSC 1.0 (no length
# header, --bare) or Heddle's length-prefixed frame to ip:port, so the
# receive path can be proven end to end without a phone. See docs/PROXY.md.
add_executable(heddle-osc-send core/tools/osc_send.cpp)
target_link_libraries(heddle-osc-send PRIVATE heddle_core)
set_target_properties(heddle-osc-send PROPERTIES OUTPUT_NAME heddle-osc-send)

# ---- JUCE host layer (opt-in) ----------------------------------------------
option(HEDDLE_BUILD_HOST "Build the JUCE host layer (VST3 plugin + smoke CLI)" OFF)
if(HEDDLE_BUILD_HOST)
  add_subdirectory(host)
endif()
