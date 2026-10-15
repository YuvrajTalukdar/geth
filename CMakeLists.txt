# GETH — Geth Engine for Tracking Hardware
# Phase 1: data collection layer + text/JSON output (no UI yet).

cmake_minimum_required(VERSION 3.20)

project(geth
  VERSION 0.1.0
  DESCRIPTION "GETH — Geth Engine for Tracking Hardware"
  LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  # Default to the sanitized build while developing: it is cheap here and
  # catches memory bugs. Use -DCMAKE_BUILD_TYPE=Release for the shipped binary.
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# For clangd / IDE indexers.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GETH_BUILD_TESTS "Build the unit tests (fetches doctest)" ON)

# ---------------------------------------------------------------------------
# Binary
# ---------------------------------------------------------------------------
set(GETH_SOURCES
  src/core/collector.cpp
  src/core/series_store.cpp
  src/sources/cpu.cpp
  src/sources/cpu_power.cpp
  src/sources/demo.cpp
  src/sources/disk.cpp
  src/sources/gpu_amd.cpp
  src/sources/gpu_backend.cpp
  src/sources/gpu_nvidia.cpp
  src/sources/gpu_registry.cpp
  src/sources/memory.cpp
  src/sources/net.cpp
  src/sources/pci_discovery.cpp
  src/sources/temps.cpp
  src/util/pci_ids.cpp
  src/util/sysfs_file.cpp)

add_executable(geth src/main.cpp ${GETH_SOURCES})
target_include_directories(geth PRIVATE src)
# Version is defined once here and passed to the code as a define.
target_compile_definitions(geth PRIVATE GETH_VERSION="${PROJECT_VERSION}")

target_compile_options(geth PRIVATE
  -Wall -Wextra -Wpedantic -Werror
  $<$<CONFIG:Debug>:-fsanitize=address,undefined -g -O1>
  $<$<CONFIG:Release>:-O2>)
target_link_options(geth PRIVATE $<$<CONFIG:Debug>:-fsanitize=address,undefined>)

# dlopen()/dlsym(): on glibc >= 2.34 these live in libc, on older distros in -ldl.
# Threads is linked so the binary is portable even though GETH is single-threaded.
find_package(Threads REQUIRED)
target_link_libraries(geth PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------------------
# Tests (doctest is header-only; fetch the tagged archive and expose the header)
# ---------------------------------------------------------------------------
if(GETH_BUILD_TESTS)
  include(FetchContent)
  # doctest is consumed header-only: FetchContent_MakeAvailable would
  # add_subdirectory() doctest's own CMakeLists and define a conflicting
  # `doctest` target, so we keep the direct Populate call.
  cmake_policy(SET CMP0169 OLD)
  FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
    URL_HASH SHA256=632ed2c05a7f53fa961381497bf8069093f0d6628c5f26286161fbd32a560186
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_GetProperties(doctest)
  if(NOT doctest_POPULATED)
    FetchContent_Populate(doctest)
  endif()

  # doctest-src/doctest/doctest.h — expose the subdirectory so
  # `#include <doctest.h>` works.
  add_library(doctest INTERFACE)
  target_include_directories(doctest INTERFACE ${doctest_SOURCE_DIR}/doctest)

  add_executable(geth_tests
    tests/alloc_count.cpp  # counting operator new (test binary only)
    tests/test_main.cpp
    tests/test_parsers.cpp
    ${GETH_SOURCES})
  target_include_directories(geth_tests PRIVATE src)
  # Absolute path so the test binary can find the fixture files from anywhere.
  target_compile_definitions(geth_tests PRIVATE
    GETH_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  target_link_libraries(geth_tests PRIVATE doctest Threads::Threads ${CMAKE_DL_LIBS})
  target_compile_options(geth_tests PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    $<$<CONFIG:Debug>:-fsanitize=address,undefined -g -O1>
    $<$<CONFIG:Release>:-O2>)
  target_link_options(geth_tests PRIVATE $<$<CONFIG:Debug>:-fsanitize=address,undefined>)

  enable_testing()
  add_test(NAME unit COMMAND geth_tests)
endif()
