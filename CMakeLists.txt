cmake_minimum_required(VERSION 3.16)
project(heddle_core VERSION 0.1.8 LANGUAGES C CXX)

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

# ---- JUCE host layer (opt-in) ----------------------------------------------
option(HEDDLE_BUILD_HOST "Build the JUCE host layer (VST3 plugin + smoke CLI)" OFF)
if(HEDDLE_BUILD_HOST)
  add_subdirectory(host)
endif()
