cmake_minimum_required(VERSION 3.20)

project(
    AudioEngine
    VERSION 0.2.0
    DESCRIPTION "Ultra-Low-Latency Audio Isolation Engine for Hearables"
    LANGUAGES CXX
)

# ── Compiler settings ─────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optimisation flags tuned for a low-power Bluetooth SoC.
# Release: aggressive optimisation + strict IEEE maths relaxation.
# Debug  : no optimisation, full debug info, sanitizers.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type: Debug | Release | RelWithDebInfo | MinSizeRel" FORCE)
endif()

add_compile_options(
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall>
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wextra>
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wpedantic>
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-unused-parameter>
    # Release: prefer speed; relax floating-point precision slightly for
    # embedded targets where full IEEE 754 is expensive.
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Release>>:-O3>
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Release>>:-ffast-math>
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Release>>:-funroll-loops>
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Debug>>:-O0>
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Debug>>:-g3>
)

# ── audio_engine static library ───────────────────────────────────────────────

add_library(audio_engine STATIC
    src/stft_processor.cpp
    src/audio_engine.cpp
    src/noise_suppressor.cpp
    src/istft_processor.cpp
    src/pipeline.cpp
)

target_include_directories(audio_engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(audio_engine PUBLIC cxx_std_20)

# Link math library on platforms that require it (e.g. Linux).
target_link_libraries(audio_engine PUBLIC m)

# ── Tests ─────────────────────────────────────────────────────────────────────

option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    enable_testing()

    # test_circular_buffer – header-only, no need to link audio_engine sources.
    add_executable(test_circular_buffer
        tests/test_circular_buffer.cpp
    )
    target_include_directories(test_circular_buffer
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_features(test_circular_buffer PRIVATE cxx_std_20)
    # Thread support for the SPSC concurrency test.
    find_package(Threads REQUIRED)
    target_link_libraries(test_circular_buffer PRIVATE Threads::Threads)

    add_test(NAME CircularBufferTests COMMAND test_circular_buffer)

    # test_stft_processor – links against the audio_engine library.
    add_executable(test_stft_processor
        tests/test_stft_processor.cpp
    )
    target_link_libraries(test_stft_processor PRIVATE audio_engine)
    target_compile_features(test_stft_processor PRIVATE cxx_std_20)

    add_test(NAME STFTProcessorTests COMMAND test_stft_processor)

    # test_noise_suppressor
    add_executable(test_noise_suppressor
        tests/test_noise_suppressor.cpp
    )
    target_link_libraries(test_noise_suppressor PRIVATE audio_engine)
    target_compile_features(test_noise_suppressor PRIVATE cxx_std_20)
    add_test(NAME NoiseSuppressorTests COMMAND test_noise_suppressor)

    # test_istft_processor
    add_executable(test_istft_processor
        tests/test_istft_processor.cpp
    )
    target_link_libraries(test_istft_processor PRIVATE audio_engine)
    target_compile_features(test_istft_processor PRIVATE cxx_std_20)
    add_test(NAME ISTFTProcessorTests COMMAND test_istft_processor)

    # test_pipeline
    add_executable(test_pipeline
        tests/test_pipeline.cpp
    )
    target_link_libraries(test_pipeline PRIVATE audio_engine)
    target_compile_features(test_pipeline PRIVATE cxx_std_20)
    add_test(NAME PipelineTests COMMAND test_pipeline)
endif()

# ── Benchmark ─────────────────────────────────────────────────────────────────

option(BUILD_BENCHMARKS "Build latency benchmarks" ON)

if(BUILD_BENCHMARKS)
    add_executable(latency_benchmark
        benchmarks/latency_benchmark.cpp
    )
    target_link_libraries(latency_benchmark PRIVATE audio_engine)
    target_compile_features(latency_benchmark PRIVATE cxx_std_20)
endif()

# ── Install rules (optional, useful for cross-compilation workflows) ──────────

include(GNUInstallDirs)

install(TARGETS audio_engine
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
