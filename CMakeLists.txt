# The portable engine (libviz), the vizrender CLI and the self-tests.
#
# This is the build for every platform's engine and for CI; the macOS app
# and the Music plugin have their own XcodeGen project under apple/ that
# consumes the same core/ sources directly. On Apple the Metal backend and
# vDSP FFT are linked; elsewhere the GPU stub (all CPU paths) and our own
# FFT — see core/platform/ for the three porting seams.
cmake_minimum_required(VERSION 3.20)
project(viz LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 17)
endif()

file(GLOB VIZ_EFFECT_SOURCES CONFIGURE_DEPENDS core/effects/*.cpp)
set(VIZ_SOURCES
  ${VIZ_EFFECT_SOURCES}
  core/script/EelVM.cpp
  core/script/EelToMsl.cpp
  core/preset/AvsPreset.cpp
  core/preset/JsonPreset.cpp
  core/audio/Analyzer.cpp
  core/audio/BeatDetector.cpp
)
if(APPLE)
  list(APPEND VIZ_SOURCES core/gpu/metal/GpuFx.mm core/platform/fft.mm)
else()
  list(APPEND VIZ_SOURCES core/gpu/GpuFxStub.cpp core/platform/fft.cpp)
endif()

add_library(viz STATIC ${VIZ_SOURCES})
target_include_directories(viz PUBLIC
  core core/effects core/script core/preset core/audio core/gpu core/platform)
target_compile_options(viz PRIVATE -Wall -Wno-deprecated-declarations -Wno-unused-function)
if(APPLE)
  target_link_libraries(viz PUBLIC
    "-framework Metal" "-framework QuartzCore" "-framework Accelerate" "-framework Foundation")
endif()

# --- tools ---------------------------------------------------------------
add_executable(vizrender core/tools/vizrender.cpp)
target_link_libraries(vizrender PRIVATE viz)

# --- self-tests ------------------------------------------------------------
enable_testing()
set(TEST_OUT ${CMAKE_BINARY_DIR}/test-out)
file(MAKE_DIRECTORY ${TEST_OUT}/effects ${TEST_OUT}/gallery ${TEST_OUT}/avs)

foreach(t eel_selftest beat_selftest fft_selftest effects_selftest avs_selftest)
  add_executable(${t} core/tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE viz)
endforeach()

add_test(NAME eel     COMMAND eel_selftest)
add_test(NAME fft     COMMAND fft_selftest)
add_test(NAME beat    COMMAND beat_selftest)
add_test(NAME effects COMMAND effects_selftest ${TEST_OUT}/effects)
add_test(NAME gallery COMMAND effects_selftest gallery ${TEST_OUT}/gallery)
add_test(NAME avs     COMMAND avs_selftest ${CMAKE_SOURCE_DIR}/presets/tests ${TEST_OUT}/avs)
