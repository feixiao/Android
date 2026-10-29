cmake_minimum_required(VERSION 3.18.1)

project("audio_agent" CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization & Architecture Flags (Section 3.2)
add_compile_options(
    -O3
    -fPIC
    -Wall
    -Wextra
    -Wno-unused-parameter
)

if(ANDROID_ABI STREQUAL "arm64-v8a")
    add_compile_options(-march=armv8.2-a+dotprod+fp16)
endif()

# Header include search directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Source files
set(AUDIO_AGENT_SOURCES
    src/CpuAffinity.cpp
    src/StreamingSentenceChunker.cpp
    src/WatchdogAndRepetition.cpp
    src/ThreeTierMemory.cpp
    src/FullDuplexAudioIO.cpp
    src/AudioAgentPipeline.cpp
    src/NativeJniBridge.cpp
)

# Check for Oboe package via Android Prefab
find_package(oboe CONFIG QUIET)

if(oboe_FOUND)
    message(STATUS "Found Oboe via Android Prefab.")
    add_definitions(-DAUDIO_AGENT_USE_OBOE)
else()
    message(STATUS "Oboe not found via Prefab, attempting FetchContent or fallback.")
endif()

# Build shared library libaudio_agent.so
add_library(audio_agent SHARED ${AUDIO_AGENT_SOURCES})

# Target link dependencies
target_link_libraries(
    audio_agent
    android
    log
    OpenSLES
)

if(oboe_FOUND)
    target_link_libraries(audio_agent oboe::oboe)
endif()
