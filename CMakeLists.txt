# GETH — Geth Engine for Tracking Hardware
# Phase 1: data collection layer + text/JSON output (no UI yet).

cmake_minimum_required(VERSION 3.20)

project(geth
  VERSION 1.0.3
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

# The installed binary name. Go Ethereum also installs a `geth` binary, so on
# a machine where both are wanted build with -DGETH_BINARY_NAME=geth-engine.
set(GETH_BINARY_NAME "geth" CACHE STRING "Name of the installed binary")

# SPDX license identifier. Defined ONCE here; the install rule and the
# packaging scripts (PKGBUILD's license=, package-arch.sh's consistency
# check) all read from this variable — change it here and nowhere else.
set(GETH_LICENSE "MIT" CACHE STRING "SPDX license identifier for GETH")

# ---------------------------------------------------------------------------
# FTXUI (terminal UI framework) — pinned to an exact tag for reproducibility.
# The tag is recorded here and in the README. Only what we need is built
# (screen + dom + component); examples/docs/tests and the install rules are
# disabled. FTXUI's headers are SYSTEM INTERFACE includes, so their warnings
# never leak into our -Werror build.
# ---------------------------------------------------------------------------
include(FetchContent)
cmake_policy(SET CMP0169 OLD)
FetchContent_Declare(ftxui
  URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v6.1.9.tar.gz
  URL_HASH SHA256=45819c1e54914783d4a1ca5633885035d74146778a1f74e1213cdb7b76340e71
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(ftxui)
if(NOT ftxui_POPULATED)
  set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Populate(ftxui)
  add_subdirectory(${ftxui_SOURCE_DIR} ${ftxui_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

# ---------------------------------------------------------------------------
# toml++ (TOML config parsing) — header-only, pinned to an exact tag. Nothing
# is built; we expose only the include directory of the fetched source.
# ---------------------------------------------------------------------------
FetchContent_Declare(tomlplusplus
  URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
  URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(tomlplusplus)
if(NOT tomlplusplus_POPULATED)
  FetchContent_Populate(tomlplusplus)
endif()

# Header-only: an INTERFACE target with the include directory.
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus INTERFACE ${tomlplusplus_SOURCE_DIR}/include)

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
  src/ui/app.cpp
  src/ui/config.cpp
  src/ui/frame.cpp
  src/ui/layout.cpp
  src/ui/panel_spec.cpp
  src/ui/term_probe.cpp
  src/ui/theme.cpp
  src/util/alloc_count.cpp
  src/util/pci_ids.cpp
  src/util/sysfs_file.cpp
  src/util/terminal.cpp
  src/util/trace.cpp)

add_executable(geth src/main.cpp ${GETH_SOURCES})
# The target stays `geth` (the setcap target references it); only the output
# and installed name follow GETH_BINARY_NAME.
set_target_properties(geth PROPERTIES OUTPUT_NAME ${GETH_BINARY_NAME})
target_include_directories(geth PRIVATE src)
# Version is defined once here and passed to the code as a define.
target_compile_definitions(geth PRIVATE GETH_VERSION="${PROJECT_VERSION}")

target_compile_options(geth PRIVATE
  -Wall -Wextra -Wpedantic -Werror
  $<$<CONFIG:Debug>:-fsanitize=address,undefined -g -O1>
  $<$<CONFIG:Release>:-O2>)
target_link_options(geth PRIVATE $<$<CONFIG:Debug>:-fsanitize=address,undefined>)

# dlopen()/dlsym(): on glibc >= 2.34 these live in libc, on older distros in -ldl.
# Threads is linked for the UI's timer thread (it only posts an event; all
# sampling/rendering runs on the UI thread).
find_package(Threads REQUIRED)
# zlib compresses the pixel-renderer image payloads (kitty o=z, PNG IDAT).
find_package(ZLIB REQUIRED)
target_link_libraries(geth PRIVATE ftxui::component tomlplusplus Threads::Threads ZLIB::ZLIB ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------------------
# RAPL access helpers (opt-in; never run by a plain build).
#
# Since kernel 5.10 the powercap energy_uj files are 0400 root:root, so
# reading CPU package power needs either root or a capability / udev rule.
# See README ("Reading CPU package power without root").
# ---------------------------------------------------------------------------
add_custom_target(setcap
  COMMAND setcap cap_dac_read_search+ep $<TARGET_FILE:geth>
  COMMENT "setcap cap_dac_read_search+ep geth  (run as root: sudo cmake --build . --target setcap)"
  VERBATIM)

add_custom_target(install-udev
  COMMAND ${CMAKE_COMMAND} -E copy
          "${CMAKE_CURRENT_SOURCE_DIR}/packaging/99-geth-rapl.rules"
          /etc/udev/rules.d/99-geth-rapl.rules
  COMMAND udevadm control --reload
  COMMAND udevadm trigger --subsystem-match=powercap
  COMMENT "Installing packaging/99-geth-rapl.rules into /etc  (run as root: sudo cmake --build . --target install-udev)"
  VERBATIM)

# ---------------------------------------------------------------------------
# Install layout (all under the install prefix, e.g. /usr or ~/.local):
#   <prefix>/bin/geth                        binary (name: GETH_BINARY_NAME)
#   <prefix>/share/geth/themes/example.toml  example custom theme
#   <prefix>/share/geth/99-geth-rapl.rules   udev rule — SHIPPED, NOT
#                                            activated; the user opts in by
#                                            copying it to /etc/udev/rules.d
#                                            (or: sudo cmake --build .
#                                            --target install-udev)
#   <prefix>/share/man/man1/geth.1.gz        man page (docs/geth.1, roff)
#   <prefix>/share/doc/geth/{README,CHANGELOG}.md
#   <prefix>/share/licenses/geth/LICENSE
# `cmake --install` never touches /etc and never touches the user's config
# (~/.config/geth). Uninstall: `cmake --build . --target uninstall`.
# ---------------------------------------------------------------------------
# Man pages ship gzipped (the convention for man1). Compress docs/geth.1 into
# the build tree at build time, then install the .gz — no shell, so paths with
# spaces are safe. `makepkg`/pacman accept an already-gzipped page as-is.
set(GETH_MAN_GZ "${CMAKE_CURRENT_BINARY_DIR}/${GETH_BINARY_NAME}.1.gz")
set(GETH_MAN_GZ_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/gzip_man.cmake")
file(WRITE "${GETH_MAN_GZ_SCRIPT}"
  "execute_process(COMMAND gzip -9 -c \"${CMAKE_CURRENT_SOURCE_DIR}/docs/geth.1\"\n"
  "  OUTPUT_FILE \"${GETH_MAN_GZ}\")\n")
add_custom_command(
  OUTPUT "${GETH_MAN_GZ}"
  COMMAND ${CMAKE_COMMAND} -P "${GETH_MAN_GZ_SCRIPT}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/docs/geth.1"
  COMMENT "Gzipping the man page"
  VERBATIM)
add_custom_target(geth-man DEPENDS "${GETH_MAN_GZ}")
add_dependencies(geth geth-man)

install(TARGETS geth RUNTIME DESTINATION bin)
install(FILES packaging/themes/example.toml DESTINATION share/geth/themes)
install(FILES packaging/99-geth-rapl.rules DESTINATION share/geth)
install(FILES "${GETH_MAN_GZ}" DESTINATION share/man/man1)
install(FILES README.md CHANGELOG.md DESTINATION share/doc/geth)
install(FILES LICENSE DESTINATION share/licenses/${GETH_BINARY_NAME}
        RENAME LICENSE)

# ---------------------------------------------------------------------------
# Uninstall (source installs): remove every file this install placed, from
# CMake's install manifest. The manifest lists only what `cmake --install`
# wrote — the user config in ~/.config/geth is never in it, so it is never
# touched. (Pacman-managed installs use `pacman -R` instead.)
# ---------------------------------------------------------------------------
# @BUILD_DIR@ is substituted at configure time (the -P script cannot receive
# it any other way — cmake -P -D only sets script-scope variables, which the
# configured text does not reference).
set(BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}")
configure_file(cmake/uninstall.cmake.in
               ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake @ONLY)
add_custom_target(uninstall
  ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
  COMMENT "Uninstalling every file listed in install_manifest.txt")

# ---------------------------------------------------------------------------
# package-arch: build the Arch .pkg.tar.zst from a clean `git archive` of the
# current tree (see tools/package-arch.sh). Needs base-devel (makepkg).
# ---------------------------------------------------------------------------
add_custom_target(package-arch
  COMMAND ${CMAKE_COMMAND} -E env bash
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/package-arch.sh
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Building the Arch package via tools/package-arch.sh")

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

  # alloc_count.cpp (the counting operator new) now lives in src/util and is
  # compiled via ${GETH_SOURCES}; the test binary proves the zero-alloc
  # sampling path, the shipped binary reports allocs/frame under --stats.
  add_executable(geth_tests
    tests/test_main.cpp
    tests/test_parsers.cpp
    tests/test_names.cpp
    tests/test_ui.cpp
    tests/test_layout.cpp
    tests/test_config.cpp
    tests/test_dashboard.cpp
    tests/test_theme.cpp
    tests/test_collector.cpp
    ${GETH_SOURCES})
  target_include_directories(geth_tests PRIVATE src)
  # Absolute path so the test binary can find the fixture files from anywhere.
  # GETH_VERSION mirrors the main target: config.cpp (shared via ${GETH_SOURCES})
  # embeds it in the written config's header comment.
  target_compile_definitions(geth_tests PRIVATE
    GETH_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    GETH_VERSION="${PROJECT_VERSION}")
  target_link_libraries(geth_tests PRIVATE doctest ftxui::component tomlplusplus Threads::Threads ZLIB::ZLIB ${CMAKE_DL_LIBS})
  target_compile_options(geth_tests PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    $<$<CONFIG:Debug>:-fsanitize=address,undefined -g -O1>
    $<$<CONFIG:Release>:-O2>)
  target_link_options(geth_tests PRIVATE $<$<CONFIG:Debug>:-fsanitize=address,undefined>)

  enable_testing()
  add_test(NAME unit COMMAND geth_tests)
endif()
