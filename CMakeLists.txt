cmake_minimum_required(VERSION 3.16)
project(weft_core VERSION 0.1.5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- core library -----------------------------------------------------------
add_library(weft_core STATIC
    core/src/osc.cpp
    core/src/config.cpp
    core/src/chain.cpp
    core/src/curve.cpp
    core/src/osc_io.cpp
    core/src/ring.cpp
)
target_include_directories(weft_core PUBLIC
  core/include
  third_party
)
if(WIN32)
  target_compile_definitions(weft_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
  # osc_io.cpp uses Winsock on Windows; propagate the import library to every
  # consumer of the static lib (tests, hosts, probes).
  target_link_libraries(weft_core PUBLIC ws2_32)
endif()

# ---- tests ------------------------------------------------------------------
enable_testing()
add_executable(weft_core_tests
  core/tests/test_osc.cpp
  core/tests/test_config.cpp
  core/tests/test_chain.cpp
  core/tests/test_curve.cpp
  core/tests/test_osc_udp.cpp
  core/tests/test_ring.cpp
)
target_link_libraries(weft_core_tests PRIVATE weft_core)
target_include_directories(weft_core_tests PRIVATE third_party/doctest)
add_test(NAME weft_core_tests COMMAND weft_core_tests)

# ---- JUCE host layer (opt-in) ----------------------------------------------
option(WEFT_BUILD_HOST "Build the JUCE host layer (VST3 plugin + smoke CLI)" OFF)
if(WEFT_BUILD_HOST)
  add_subdirectory(host)
endif()
