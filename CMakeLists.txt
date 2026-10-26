# backbone-core: the engine shared by the LayerTwo-family compat clients (Thunder,
# BitNames, BitAssets, zSide, Truthcoin). Builds standalone (core tests) or as a
# subdirectory of a chain repository (`add_subdirectory(core)`), where it defines:
#   bt_blake3, bt_univalue   vendored third-party code
#   bt_core                  consensus-neutral codecs, crypto, store, mainchain types, JSON-RPC
#   bt_net                   QUIC, gRPC, enforcer client (the only part linking TLS)
#   bt_test_harness          header + main() of the test runner (INTERFACE sources)
#   bt_core_test_cases       the core's own test cases, linkable into a chain's test binary
cmake_minimum_required(VERSION 3.22)
project(backbone_core VERSION 0.1.0 LANGUAGES C CXX)

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
  endif()
endif()

option(BT_STATIC "Link third-party libraries statically" ON)
option(BT_BUILD_TESTS "Build unit tests" ON)
option(BT_WITH_NET "Build the QUIC/gRPC/RPC node (needs OpenSSL>=3.5, ngtcp2, nghttp2)" ON)
set(BT_DEPENDS_PREFIX "" CACHE PATH "Prefix with our pinned third-party builds (depends/)")
set(BT_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "backbone-core source dir")
if(NOT BT_DEPENDS_PREFIX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/depends/built/linux" AND NOT CMAKE_CROSSCOMPILING)
  set(BT_DEPENDS_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/depends/built/linux" CACHE PATH "" FORCE)
endif()
if(BT_DEPENDS_PREFIX)
  list(PREPEND CMAKE_PREFIX_PATH "${BT_DEPENDS_PREFIX}")
  list(PREPEND CMAKE_FIND_ROOT_PATH "${BT_DEPENDS_PREFIX}")
endif()

# Warnings for our code; -ffp-contract=off is consensus-relevant (f64 block limits must
# equal Rust's non-fused results) and is propagated to every consumer of bt_core.
set(BT_WARNINGS -Wall -Wextra -Wno-unused-parameter)
add_compile_options(${BT_WARNINGS} $<$<COMPILE_LANGUAGE:C,CXX>:-ffp-contract=off>)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET GLOBAL libsodium)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET GLOBAL sqlite3)

# ---------------------------------------------------------------- third party
add_library(bt_blake3 STATIC
  third_party/blake3/blake3.c
  third_party/blake3/blake3_dispatch.c
  third_party/blake3/blake3_portable.c)
target_include_directories(bt_blake3 PUBLIC third_party/blake3)
target_compile_definitions(bt_blake3 PRIVATE BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512 BLAKE3_USE_NEON=0)

add_library(bt_univalue STATIC
  third_party/univalue/lib/univalue.cpp
  third_party/univalue/lib/univalue_get.cpp
  third_party/univalue/lib/univalue_read.cpp
  third_party/univalue/lib/univalue_write.cpp)
target_include_directories(bt_univalue PUBLIC third_party/univalue/include)
target_compile_options(bt_univalue PRIVATE -w)

# ---------------------------------------------------------------- core
add_subdirectory(libs/core)
target_compile_features(bt_core PUBLIC cxx_std_20)
target_compile_options(bt_core PUBLIC -ffp-contract=off)

if(BT_WITH_NET)
  # The app layer of the drop-in nodes (doc/app.md of the drop-in repositories): the app RPC, the host of the pages.
  add_subdirectory(libs/appui)
  add_executable(bt-enforcer-probe tools/enforcer_probe.cpp)
  target_link_libraries(bt-enforcer-probe PRIVATE bt_net)
endif()

# ---------------------------------------------------------------- tests
if(BT_BUILD_TESTS)
  add_library(bt_test_harness INTERFACE)
  target_sources(bt_test_harness INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tests/main.cpp)
  target_include_directories(bt_test_harness INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

  add_library(bt_core_test_cases OBJECT tests/test_core.cpp tests/test_core_ext.cpp)
  target_link_libraries(bt_core_test_cases PUBLIC bt_core)
  target_include_directories(bt_core_test_cases PRIVATE tests)
  target_compile_definitions(bt_core_test_cases PRIVATE BT_CORE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  if(BT_WITH_NET)
    target_sources(bt_core_test_cases PRIVATE tests/test_grpc.cpp tests/test_security.cpp tests/test_ecies.cpp tests/test_appui.cpp tests/test_quic_agent.cpp)
    target_link_libraries(bt_core_test_cases PUBLIC bt_net bt_appui)
  endif()

  if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_executable(bt_core_tests)
    target_link_libraries(bt_core_tests PRIVATE bt_test_harness bt_core_test_cases)
    target_compile_definitions(bt_core_tests PRIVATE BT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    add_test(NAME bt_core_tests COMMAND bt_core_tests)
  endif()
endif()
