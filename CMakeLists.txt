cmake_minimum_required(VERSION 3.28)
project(Helix.Audio LANGUAGES CXX)

if(NOT COMMAND helix_add_library)
    include("${CMAKE_CURRENT_SOURCE_DIR}/External/Helix.CMake/Helix.cmake")
endif()

helix_require(Core)
helix_require(Platform)
helix_add_library(helix_audio ALIAS Audio PUBLIC_DEPS Helix::Core PRIVATE_DEPS Helix::Platform)
# Copied public domain code, see THIRD-PARTY.txt.
target_include_directories(helix_audio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty)

helix_add_test_suite(helix_audio_tests DEPS Helix::Audio)
helix_add_benchmark_suite(helix_audio_bench DEPS Helix::Audio)
if(TARGET helix_audio_bench)
    # The benchmarks decode the generated test sounds.
    target_include_directories(helix_audio_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Tests)
endif()
helix_add_fuzz_suite(helix_audio_fuzz DEPS Helix::Audio ROUNDS 1000)

# Rounds the long fuzz runs found faults in, replayed on every test run.
if(TARGET helix_audio_fuzz)
    add_test(NAME helix_audio_fuzz_round_2394233 COMMAND helix_audio_fuzz --target=Vorbis --first=2394233 --rounds=1)
endif()
