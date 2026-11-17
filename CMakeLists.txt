cmake_minimum_required(VERSION 3.20)
project(aselsan_rehis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single-config generators (Makefiles, Ninja): build optimized unless asked otherwise.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()

option(REHIS_BUILD_TESTS "Build the GoogleTest unit tests (needs GoogleTest: installed, or downloaded at configure time)" ON)
option(REHIS_BUILD_BENCH "Build the benchmark (rehis_bench) and, on macOS, the heap check (rehis_heapcheck)" ON)
option(REHIS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

find_package(Threads REQUIRED)

# Warnings for the project's own targets (not for GoogleTest).
function(rehis_set_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(REHIS_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

# Product code uses no exceptions (RTOS rule): a throw/try/catch in it is a compile error.
# Only for targets built from product sources; tests and bench tools keep exceptions
# (std::string, std::vector, GoogleTest).
function(rehis_disable_exceptions target)
    target_compile_options(${target} PRIVATE -fno-exceptions)
endfunction()

set(REHIS_CORE_SOURCES
        src/Constants.h
        src/Buffer.h
        src/Threat.cpp
        src/Threat.h
        src/ThreatLogger.cpp
        src/ThreatLogger.h
)

# ---------------- Core library ----------------
# Classes under test, compiled once and shared by the app and the tests.
add_library(rehis_core STATIC ${REHIS_CORE_SOURCES})
target_include_directories(rehis_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rehis_core PUBLIC Threads::Threads)
rehis_set_warnings(rehis_core)
rehis_disable_exceptions(rehis_core)

# ---------------- Application ----------------
add_executable(aselsan_rehis src/main.cpp)
target_link_libraries(aselsan_rehis PRIVATE rehis_core)
rehis_set_warnings(aselsan_rehis)
rehis_disable_exceptions(aselsan_rehis)

# ---------------- Benchmark ----------------
if(REHIS_BUILD_BENCH)
    # Same sources, built with THREAT_LOGGER_PROFILING: timing probes inside ThreatLogger
    # are compiled in. The product library above never sees the flag.
    # Always optimized (-O2): -O0 Debug timings would not represent the real system.
    add_library(rehis_core_profiled STATIC ${REHIS_CORE_SOURCES} bench/LoggerProfiler.cpp bench/LoggerProfiler.h)
    target_include_directories(rehis_core_profiled PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/bench)
    target_compile_definitions(rehis_core_profiled PUBLIC THREAT_LOGGER_PROFILING)
    target_compile_options(rehis_core_profiled PRIVATE -O2)
    target_link_libraries(rehis_core_profiled PUBLIC Threads::Threads)
    rehis_set_warnings(rehis_core_profiled)
    rehis_disable_exceptions(rehis_core_profiled)    # measure the same build as the product

    add_executable(rehis_bench bench/LoggerBench.cpp)
    target_compile_options(rehis_bench PRIVATE -O2)
    target_link_libraries(rehis_bench PRIVATE rehis_core_profiled)
    rehis_set_warnings(rehis_bench)

    # Heap check (macOS only): counts heap allocations after init.
    # The counter is a dylib that interposes malloc and friends.
    if(APPLE)
        add_library(malloc_counter SHARED bench/heapcheck/MallocCounter.cpp)
        add_executable(rehis_heapcheck bench/heapcheck/HeapCheck.cpp)
        target_link_libraries(rehis_heapcheck PRIVATE rehis_core malloc_counter)
        rehis_set_warnings(rehis_heapcheck)
    endif()
endif()

# ---------------- Tests ----------------
if(REHIS_BUILD_TESTS)
    # 1) An installed GoogleTest (e.g. apt install libgtest-dev, brew install googletest).
    # 2) Otherwise download it at configure time. Offline: unzip the release somewhere and pass
    #    -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<that folder>.
    find_package(GTest 1.11 CONFIG QUIET)
    if(GTest_FOUND)
        message(STATUS "GoogleTest: using installed version ${GTest_VERSION}")
    else()
        message(STATUS "GoogleTest: not installed, fetching v1.17.0")
        include(FetchContent)
        if(POLICY CMP0135)
            cmake_policy(SET CMP0135 NEW)       # extracted files get the extraction time
        endif()
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
                googletest
                URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
        )
        FetchContent_MakeAvailable(googletest)
    endif()

    enable_testing()

    add_executable(rehis_tests
            tests/BufferTest.cpp
            tests/ThreatTest.cpp
            tests/ThreatLoggerTest.cpp
            tests/ThreatLoggerBehaviorTest.cpp
            tests/ThreatLoggerTestUtils.h
    )
    target_link_libraries(rehis_tests PRIVATE rehis_core GTest::gtest_main)
    rehis_set_warnings(rehis_tests)

    include(GoogleTest)
    gtest_discover_tests(rehis_tests DISCOVERY_TIMEOUT 30)
endif()
