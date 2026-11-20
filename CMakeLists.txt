cmake_minimum_required(VERSION 3.20)
project(lobsim CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOBSIM_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(LOBSIM_WERROR "Treat warnings in our own targets as errors" ON)

# Global, and before the dependencies are fetched, so they are instrumented too.
if(LOBSIM_SANITIZE)
  set(_lobsim_san -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_compile_options(${_lobsim_san})
  add_link_options(${_lobsim_san})
endif()

# Warnings apply to our targets only, through this interface target.
add_library(lobsim_options INTERFACE)
target_compile_options(lobsim_options INTERFACE -Wall -Wextra -Wpedantic)
if(LOBSIM_WERROR)
  target_compile_options(lobsim_options INTERFACE -Werror)
endif()

include(FetchContent)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

set(_lobsim_fc_system "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
  set(_lobsim_fc_system SYSTEM)
endif()

FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
  GIT_SHALLOW TRUE
  ${_lobsim_fc_system})
FetchContent_Declare(benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.8.3
  GIT_SHALLOW TRUE
  ${_lobsim_fc_system})
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE
  ${_lobsim_fc_system})
FetchContent_MakeAvailable(googletest benchmark nlohmann_json)

# Mark dependency include directories SYSTEM (needed below CMake 3.25).
foreach(_t gtest gtest_main gmock benchmark nlohmann_json)
  if(TARGET ${_t})
    get_target_property(_inc ${_t} INTERFACE_INCLUDE_DIRECTORIES)
    if(_inc)
      set_target_properties(${_t} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
    endif()
  endif()
endforeach()

add_library(lob STATIC src/lob/order_book.cpp src/lob/reference_book.cpp)
target_include_directories(lob PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lob PUBLIC cxx_std_20)
target_link_libraries(lob PRIVATE lobsim_options)

# Shared differential driver and the crash-file decoder (upgrade 1).
add_subdirectory(fuzz)

# The simulator. It depends on lob; lob does not depend on it.
add_library(sim STATIC
  src/sim/as_market_maker.cpp
  src/sim/config.cpp
  src/sim/exchange.cpp
  src/sim/informed_trader.cpp
  src/sim/metrics.cpp
  src/sim/noise_trader.cpp
  src/sim/order_log.cpp
  src/sim/replay.cpp
  src/sim/simulator.cpp)
target_link_libraries(sim PUBLIC lob nlohmann_json::nlohmann_json PRIVATE lobsim_options)
# No fused multiply-add: floating-point results must not depend on the build type. PUBLIC,
# because the generators and the fundamental value are inline in headers and are compiled
# into whatever uses them.
target_compile_options(sim PUBLIC -ffp-contract=off)

add_executable(lobsim_run apps/run_main.cpp)
target_link_libraries(lobsim_run PRIVATE sim lobsim_options)

add_executable(lobsim_replay apps/replay_main.cpp)
target_link_libraries(lobsim_replay PRIVATE sim lobsim_options)

enable_testing()
include(GoogleTest)

add_executable(lob_tests
  tests/unit/smoke_test.cpp
  tests/unit/matching_test.cpp
  tests/unit/fok_postonly_test.cpp
  tests/unit/validation_test.cpp
  tests/unit/cancel_modify_test.cpp
  tests/unit/stp_test.cpp
  tests/unit/queries_test.cpp
  tests/unit/conservation_test.cpp
  tests/unit/pool_test.cpp
  tests/unit/order_book_validate_test.cpp)
target_include_directories(lob_tests PRIVATE tests)
target_link_libraries(lob_tests PRIVATE lob lobsim_options GTest::gtest_main)
gtest_discover_tests(lob_tests)

# Simulator tests (SPEC 5, 9 item 5). Every test is deterministic and writes no fixed file name.
add_executable(sim_tests
  tests/unit/sim_rng_test.cpp
  tests/unit/sim_queue_test.cpp
  tests/unit/sim_order_log_test.cpp
  tests/unit/sim_config_test.cpp
  tests/unit/sim_exchange_test.cpp
  tests/unit/sim_kernel_test.cpp
  tests/unit/sim_noise_trader_test.cpp
  tests/unit/sim_informed_trader_test.cpp
  tests/unit/sim_as_market_maker_test.cpp
  tests/unit/sim_mm_integration_test.cpp
  tests/unit/sim_integration_test.cpp
  tests/unit/sim_metrics_test.cpp
  tests/determinism.cpp
  tests/replay.cpp)
target_include_directories(sim_tests PRIVATE tests)
target_link_libraries(sim_tests PRIVATE sim lobsim_options GTest::gtest_main)
target_compile_definitions(sim_tests PRIVATE LOBSIM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
gtest_discover_tests(sim_tests)

# Command line: lobsim_run, then lobsim_replay on its log; the hash must match summary.json.
add_test(NAME cli_run_then_replay
  COMMAND ${CMAKE_COMMAND}
    -DRUN=$<TARGET_FILE:lobsim_run> -DREPLAY=$<TARGET_FILE:lobsim_replay>
    -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/configs/default.json
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/cli_run_then_replay
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_replay.cmake)

# Differential fuzz test (SPEC 4): OrderBook vs ReferenceBook, event for event.
add_executable(fuzz_diff tests/fuzz_diff.cpp)
target_include_directories(fuzz_diff PRIVATE tests)
target_link_libraries(fuzz_diff PRIVATE lob lobsim_options)
# The default mode (200 seeds x 10,000 messages) split by seed range into separate
# entries, so the sanitize build can run them in parallel (CMakePresets test jobs).
foreach(_first RANGE 1 181 20)
  math(EXPR _last "${_first} + 19")
  add_test(NAME fuzz_diff_seeds_${_first}_${_last} COMMAND fuzz_diff --first-seed ${_first} --seeds 20)
endforeach()

# Regression test (upgrade 1, 3.4): every file of fuzz/regressions/diff and fuzz/corpus/diff
# through run_diff. The files are listed when the test runs, so a new file needs no rebuild
# and no re-configure; hence no gtest_discover_tests here (its cached list would skip it).
add_executable(fuzz_regression_tests tests/fuzz_regressions.cpp)
target_link_libraries(fuzz_regression_tests PRIVATE fuzz_driver lobsim_options GTest::gtest_main)
target_compile_definitions(fuzz_regression_tests PRIVATE LOBSIM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME fuzz_regressions_diff COMMAND fuzz_regression_tests --gtest_filter=Diff/*)

# Decoder unit tests (upgrade 1, 3.1).
add_executable(fuzz_decode_tests tests/unit/fuzz_decode_test.cpp)
target_link_libraries(fuzz_decode_tests PRIVATE fuzz_driver lobsim_options GTest::gtest_main)
gtest_discover_tests(fuzz_decode_tests)

# Benchmarks (SPEC 7). Meaningful only in the release preset; the debug and sanitize
# builds compile them and run a short smoke test of the throughput harness.
add_executable(lobsim_bench apps/bench_main.cpp)
target_link_libraries(lobsim_bench PRIVATE lob lobsim_options)
string(TOUPPER "${CMAKE_BUILD_TYPE}" _lobsim_bt)
# The flags line the harness prints. Assembled by hand from the same pieces used above.
set(_lobsim_flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_lobsim_bt}} -std=c++20 -Wall -Wextra -Wpedantic")
if(LOBSIM_WERROR)
  string(APPEND _lobsim_flags " -Werror")
endif()
if(LOBSIM_SANITIZE)
  list(JOIN _lobsim_san " " _lobsim_san_text)
  string(APPEND _lobsim_flags " ${_lobsim_san_text}")
endif()
string(STRIP "${_lobsim_flags}" _lobsim_flags)
target_compile_definitions(lobsim_bench PRIVATE
  "LOBSIM_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\""
  "LOBSIM_CXX_FLAGS=\"${_lobsim_flags}\"")
add_test(NAME lobsim_bench_smoke COMMAND lobsim_bench --messages 20000 --ref-messages 2000)

add_executable(micro_bench bench/micro_bench.cpp)
target_link_libraries(micro_bench PRIVATE lob lobsim_options benchmark::benchmark_main)
