cmake_minimum_required(VERSION 3.20)
project(lobsim CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LOBSIM_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(LOBSIM_FUZZ "Build libFuzzer targets" OFF)

# A fuzz build defaults to Debug: NDEBUG stays off, so the engine's own validate() calls run.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  if(LOBSIM_FUZZ)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  else()
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  endif()
endif()

option(LOBSIM_WERROR "Treat warnings in our own targets as errors" ON)

# Global, and before the dependencies are fetched, so they are instrumented too.
if(LOBSIM_SANITIZE)
  set(_lobsim_san -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_compile_options(${_lobsim_san})
  add_link_options(${_lobsim_san})
endif()

# libFuzzer build: its own build directory (build-fuzz/), only lob, the driver and the fuzz
# executables. The engine and the driver are instrumented; nothing instrumented is linked
# into a non-fuzzer executable.
if(LOBSIM_FUZZ)
  if(LOBSIM_SANITIZE)
    message(FATAL_ERROR "LOBSIM_FUZZ and LOBSIM_SANITIZE are mutually exclusive: a fuzz build already includes AddressSanitizer and UBSan")
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "LOBSIM_FUZZ needs LLVM Clang with libFuzzer (found ${CMAKE_CXX_COMPILER_ID}); use LLVM clang++, on macOS $(brew --prefix llvm)/bin/clang++")
  endif()
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all
    -g -O1 -fno-omit-frame-pointer)
  add_compile_definitions(LOBSIM_FUZZ_BUILD=1)
endif()

# One injected OrderBook bug for the fuzz canaries (src/lob/order_book.cpp). Anything but a known
# name, or any name without LOBSIM_FUZZ, stops here: a typo must never build a bug-free canary.
set(LOBSIM_INJECT_BUG "" CACHE STRING "Fuzz canary: LOBSIM_BUG_TAKER_PRICE, LOBSIM_BUG_MODIFY_KEEPS_PRIORITY or LOBSIM_BUG_FOK_IGNORES_STP")
if(NOT LOBSIM_INJECT_BUG STREQUAL "")
  if(NOT LOBSIM_INJECT_BUG MATCHES "^(LOBSIM_BUG_TAKER_PRICE|LOBSIM_BUG_MODIFY_KEEPS_PRIORITY|LOBSIM_BUG_FOK_IGNORES_STP)$")
    message(FATAL_ERROR "LOBSIM_INJECT_BUG='${LOBSIM_INJECT_BUG}' is not one of LOBSIM_BUG_TAKER_PRICE, LOBSIM_BUG_MODIFY_KEEPS_PRIORITY, LOBSIM_BUG_FOK_IGNORES_STP")
  endif()
  if(NOT LOBSIM_FUZZ)
    message(FATAL_ERROR "LOBSIM_INJECT_BUG needs -DLOBSIM_FUZZ=ON: an injected bug is only allowed in a fuzz build")
  endif()
  add_compile_definitions(${LOBSIM_INJECT_BUG}=1)
endif()

# One injected replayer bug for the replay canaries (src/replay/replayer.cpp). The build holds only
# lob, replay and lobsim_lobster (nobody should run the normal suite with a bug compiled in). An
# unknown name stops here, and so does a combination with the fuzz build or the OrderBook bugs.
set(LOBSIM_REPLAY_BUG "" CACHE STRING "Replay canary: DIRECTION_IS_AGGRESSOR, APPLY_HIDDEN or NO_SEED")
if(NOT LOBSIM_REPLAY_BUG STREQUAL "")
  if(NOT LOBSIM_REPLAY_BUG MATCHES "^(DIRECTION_IS_AGGRESSOR|APPLY_HIDDEN|NO_SEED)$")
    message(FATAL_ERROR "LOBSIM_REPLAY_BUG='${LOBSIM_REPLAY_BUG}' is not one of DIRECTION_IS_AGGRESSOR, APPLY_HIDDEN, NO_SEED")
  endif()
  if(LOBSIM_FUZZ OR NOT LOBSIM_INJECT_BUG STREQUAL "")
    message(FATAL_ERROR "LOBSIM_REPLAY_BUG cannot be combined with LOBSIM_FUZZ or LOBSIM_INJECT_BUG: one injected bug per build")
  endif()
  add_compile_definitions(LOBSIM_REPLAY_BUG_${LOBSIM_REPLAY_BUG}=1 LOBSIM_REPLAY_BUG_BUILD=1)
endif()

# Warnings apply to our targets only, through this interface target.
add_library(lobsim_options INTERFACE)
target_compile_options(lobsim_options INTERFACE -Wall -Wextra -Wpedantic)
if(LOBSIM_WERROR)
  target_compile_options(lobsim_options INTERFACE -Werror)
endif()

if(NOT LOBSIM_FUZZ AND NOT LOBSIM_REPLAY_BUG)
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
endif()

add_library(lob STATIC src/lob/order_book.cpp src/lob/reference_book.cpp)
target_include_directories(lob PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lob PUBLIC cxx_std_20)
target_link_libraries(lob PRIVATE lobsim_options)

# The LOBSTER line parsers and the replayer (upgrade 2), lob only. The fuzz build compiles the
# parsers, which the fuzzers use, and not the replayer (D-186).
add_library(replay STATIC src/replay/lobster.cpp src/replay/lobster_book.cpp)
if(NOT LOBSIM_FUZZ)
  target_sources(replay PRIVATE src/replay/replayer.cpp src/replay/replayer_match.cpp
    src/replay/replayer_json.cpp src/replay/replayer_text.cpp)
endif()
target_link_libraries(replay PUBLIC lob PRIVATE lobsim_options)

# Replay canary build (LOBSIM_REPLAY_BUG): the replayer with one injected bug, and one ctest entry
# per hand-written fixture that must catch it, i.e. whose strict replay must end with exit code 2.
if(LOBSIM_REPLAY_BUG)
  add_executable(lobsim_lobster apps/lobster_main.cpp)
  target_link_libraries(lobsim_lobster PRIVATE replay lobsim_options)
  enable_testing()
  set(_canary_fixtures_DIRECTION_IS_AGGRESSOR basic_types burst matching hidden_exec)
  set(_canary_fixtures_APPLY_HIDDEN hidden_exec)
  set(_canary_fixtures_NO_SEED basic_types burst matching hidden_exec)
  foreach(_fixture IN LISTS _canary_fixtures_${LOBSIM_REPLAY_BUG})
    add_test(NAME replay_canary_${LOBSIM_REPLAY_BUG}_${_fixture}
      COMMAND ${CMAKE_COMMAND}
        -DLOBSTER=$<TARGET_FILE:lobsim_lobster>
        -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/lobster/${_fixture}
        -DLABEL=${LOBSIM_REPLAY_BUG}/${_fixture}
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/replay_canary_${_fixture}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_canary_check.cmake)
  endforeach()
  return()
endif()

# The orders.log codec and the replay: they need only lob, so the fuzz build compiles them too.
add_library(orderlog STATIC src/sim/order_log.cpp src/sim/replay.cpp)
target_link_libraries(orderlog PUBLIC lob PRIVATE lobsim_options)

# Shared differential driver and the crash-file decoder (upgrade 1).
add_subdirectory(fuzz)

if(LOBSIM_FUZZ)
  return()
endif()

# The simulator. It depends on lob; lob does not depend on it.
add_library(sim STATIC
  src/sim/agents/calibrated_flow.cpp
  src/sim/as_market_maker.cpp
  src/sim/config.cpp
  src/sim/exchange.cpp
  src/sim/informed_trader.cpp
  src/sim/metrics.cpp
  src/sim/noise_trader.cpp
  src/sim/simulator.cpp)
target_link_libraries(sim PUBLIC lob orderlog calibfile nlohmann_json::nlohmann_json PRIVATE lobsim_options)
# No fused multiply-add: floating-point results must not depend on the build type. PUBLIC,
# because the generators and the fundamental value are inline in headers and are compiled
# into whatever uses them.
target_compile_options(sim PUBLIC -ffp-contract=off)

add_executable(lobsim_run apps/run_main.cpp)
target_link_libraries(lobsim_run PRIVATE sim lobsim_options)

add_executable(lobsim_replay apps/replay_main.cpp)
target_link_libraries(lobsim_replay PRIVATE sim lobsim_options)

# LOBSTER replay (upgrade 2): the replayer, and the exporter that turns a simulator run into LOBSTER files.
add_executable(lobsim_lobster apps/lobster_main.cpp)
target_link_libraries(lobsim_lobster PRIVATE replay lobsim_options)
# The exporter's core is a library (upgrade 3, C5) so that `lobsim_calibrate validate` can export simulations in-process.
add_library(lobster_export STATIC src/sim/lobster_export.cpp)
target_link_libraries(lobster_export PUBLIC orderlog replay lob PRIVATE lobsim_options)
add_executable(lobsim_export_lobster apps/export_lobster_main.cpp)
target_link_libraries(lobsim_export_lobster PRIVATE lobster_export lobsim_options)

enable_testing()
add_subdirectory(viz)
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
  tests/unit/order_book_validate_test.cpp
  tests/unit/builder_test.cpp)
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
  tests/unit/sim_calibrated_flow_test.cpp
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

# LOBSTER line parsers and the replayer on the hand-written fixtures (upgrade 2).
add_executable(replay_tests tests/unit/lobster_parse_test.cpp tests/unit/lobster_replay_test.cpp)
target_link_libraries(replay_tests PRIVATE replay lobsim_options nlohmann_json::nlohmann_json GTest::gtest_main)
target_compile_definitions(replay_tests PRIVATE LOBSIM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
gtest_discover_tests(replay_tests)

# The calibration file reader/writer (upgrade 3).
add_library(calibfile STATIC src/calib/calibration.cpp)
target_include_directories(calibfile PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(calibfile PUBLIC cxx_std_20)
target_link_libraries(calibfile PUBLIC nlohmann_json::nlohmann_json PRIVATE lobsim_options)

add_executable(calib_tests tests/unit/calib_file_test.cpp)
target_link_libraries(calib_tests PRIVATE calibfile lobsim_options GTest::gtest_main)
target_compile_definitions(calib_tests PRIVATE LOBSIM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
gtest_discover_tests(calib_tests)

# The estimator (upgrade 3, C1): a replay observer that measures a LOBSTER day into a calibration
# file, and lobsim_calibrate. The commit it was built from is regenerated at every build.
find_package(Git QUIET)
add_custom_target(calib_git_commit
  COMMAND ${CMAKE_COMMAND} -DGIT=${GIT_EXECUTABLE} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated/calib_git_commit.hpp
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/git_commit.cmake
  BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/calib_git_commit.hpp
  VERBATIM)
add_library(calib STATIC src/calib/estimator.cpp src/calib/estimator_stats.cpp src/calib/estimator_report.cpp
  src/calib/sha256.cpp src/calib/build_commit.cpp)
add_dependencies(calib calib_git_commit)
target_include_directories(calib PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(calib PUBLIC replay lob calibfile PRIVATE lobsim_options)

add_executable(lobsim_calibrate apps/calibrate_main.cpp apps/calibrate_validate.cpp)
target_include_directories(lobsim_calibrate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
target_link_libraries(lobsim_calibrate PRIVATE calib calibvalidate lobsim_options)
target_compile_definitions(lobsim_calibrate PRIVATE LOBSIM_PLOT_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/tools/plot_calibration.py")

add_executable(calib_estimator_tests tests/unit/calib_estimator_test.cpp tests/unit/replay_observer_test.cpp)
target_link_libraries(calib_estimator_tests PRIVATE calib lobsim_options GTest::gtest_main)
target_compile_definitions(calib_estimator_tests PRIVATE LOBSIM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
gtest_discover_tests(calib_estimator_tests)

# lobsim_calibrate estimate on a hand-written fixture: exit codes 0, 1 and 3, and a file that loads.
add_test(NAME calibrate_cli
  COMMAND ${CMAKE_COMMAND}
    -DCALIBRATE=$<TARGET_FILE:lobsim_calibrate>
    -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/calib/km_example
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/calibrate_cli
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/calibrate_cli.cmake)

# Closed loop (upgrade 2, R4): a 60 s simulator run exported to LOBSTER files and replayed. Unfiltered:
# strict mode must be exact on every row at depth 10. Filtered to the top-10 window: resync mode
# must make no correction at level 1. Everything is generated in the build directory.
foreach(_variant unfiltered filtered)
  add_test(NAME lobster_closed_loop_${_variant}
    COMMAND ${CMAKE_COMMAND}
      -DRUN=$<TARGET_FILE:lobsim_run> -DEXPORT=$<TARGET_FILE:lobsim_export_lobster>
      -DLOBSTER=$<TARGET_FILE:lobsim_lobster>
      -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/configs/default.json
      -DVARIANT=${_variant}
      -DOUT=${CMAKE_CURRENT_BINARY_DIR}/lobster_closed_loop_${_variant}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/lobster_closed_loop.cmake)
endforeach()
# Fake-day mode (upgrade 2b, D3): the LOBSTER-like export is deterministic, refuses data/lobster, and
# the replay counts exactly the injected hidden executions and halt.
add_test(NAME lobster_like_fake_day
  COMMAND ${CMAKE_COMMAND}
    -DRUN=$<TARGET_FILE:lobsim_run> -DEXPORT=$<TARGET_FILE:lobsim_export_lobster>
    -DLOBSTER=$<TARGET_FILE:lobsim_lobster>
    -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/configs/default.json
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/lobster_like_fake_day
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/lobster_like.cmake)
# Calibrated background flow (upgrade 3, C2): a hand-made calibration drives the whole market, the run is
# exported and replayed clean, and the message rates are near the configured ones.
add_test(NAME calibrated_flow_closed_loop
  COMMAND ${CMAKE_COMMAND}
    -DRUN=$<TARGET_FILE:lobsim_run> -DEXPORT=$<TARGET_FILE:lobsim_export_lobster>
    -DLOBSTER=$<TARGET_FILE:lobsim_lobster>
    -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/calib/flow_small_run.json
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/calibrated_flow_closed_loop
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/calibrated_flow_closed_loop.cmake)
add_test(NAME lobster_export_fixture
  COMMAND ${CMAKE_COMMAND}
    -DEXPORT=$<TARGET_FILE:lobsim_export_lobster> -DLOBSTER=$<TARGET_FILE:lobsim_lobster>
    -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/lobster_export
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/lobster_export_fixture
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/lobster_export_fixture.cmake)

# Missing data (exit code 3) and scripts/run_real_data.sh (upgrade 2b): no data, and one fixture pair.
add_test(NAME real_data_script
  COMMAND ${CMAKE_COMMAND}
    -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_real_data.sh -DLOBSTER=$<TARGET_FILE:lobsim_lobster>
    -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/lobster/matching
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/real_data_script
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_real_data.cmake)

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
add_test(NAME fuzz_regressions_diff_builder COMMAND fuzz_regression_tests --gtest_filter=DiffBuilder/*)
add_test(NAME fuzz_regressions_replay_parse COMMAND fuzz_regression_tests --gtest_filter=ReplayParse/*)
add_test(NAME fuzz_regressions_lobster_parse COMMAND fuzz_regression_tests --gtest_filter=LobsterParse/*)
# A gtest filter that matches nothing exits 0; this one fails if the seeds are not found.
add_test(NAME fuzz_regressions_corpus_found COMMAND fuzz_regression_tests --gtest_filter=FuzzCorpus.*)

# The committed seeds must equal the output of fuzz/make_seeds.py.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME fuzz_seeds_up_to_date
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/make_seeds.py
      --check ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/diff
      --check-builder ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/diff_builder)
  # Dress rehearsal on a short synthetic fake day (upgrade 2b, D5): the D5 table without the
  # price-window filter, and the audit of every discrepancy with it.
  add_test(NAME lobster_dress_rehearsal
    COMMAND ${CMAKE_COMMAND}
      -DRUN=$<TARGET_FILE:lobsim_run> -DEXPORT=$<TARGET_FILE:lobsim_export_lobster>
      -DLOBSTER=$<TARGET_FILE:lobsim_lobster> -DPYTHON=${Python3_EXECUTABLE}
      -DAUDIT=${CMAKE_CURRENT_SOURCE_DIR}/tools/lobster_audit.py
      -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/configs/default.json
      -DOUT=${CMAKE_CURRENT_BINARY_DIR}/lobster_dress_rehearsal
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/lobster_dress_rehearsal.cmake)
  # The audit must fail on a discrepancy it cannot explain and on a number it cannot reproduce.
  add_test(NAME lobster_audit_self_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/lobster_audit_test.py
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/lobster_audit.py $<TARGET_FILE:lobsim_lobster>
      ${CMAKE_CURRENT_BINARY_DIR}/lobster_audit_self_test)
endif()

# Decoder unit tests (upgrade 1, 3.1).
add_executable(fuzz_decode_tests tests/unit/fuzz_decode_test.cpp tests/unit/fuzz_replay_parse_test.cpp)
target_link_libraries(fuzz_decode_tests PRIVATE fuzz_driver lobsim_options GTest::gtest_main)
target_compile_definitions(fuzz_decode_tests PRIVATE LOBSIM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
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

# The market fingerprint (upgrade 3, C4): one statistics module for LOBSTER data and exported simulations.
add_library(fingerprint STATIC src/calib/fingerprint.cpp src/calib/fingerprint_stats.cpp)
target_include_directories(fingerprint PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fingerprint PUBLIC replay lob calibfile nlohmann_json::nlohmann_json PRIVATE lobsim_options)
add_executable(lobsim_fingerprint apps/fingerprint_main.cpp)
target_link_libraries(lobsim_fingerprint PRIVATE fingerprint lobsim_options)
add_test(NAME lobster_price_scale
  COMMAND ${CMAKE_COMMAND}
    -DEXPORT=$<TARGET_FILE:lobsim_export_lobster>
    -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/lobster_export
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/lobster_price_scale
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/lobster_price_scale.cmake)
add_executable(fingerprint_tests tests/unit/fingerprint_test.cpp)
target_link_libraries(fingerprint_tests PRIVATE fingerprint lobsim_options GTest::gtest_main)
target_compile_definitions(fingerprint_tests PRIVATE LOBSIM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
gtest_discover_tests(fingerprint_tests)

# Out-of-sample validation (upgrade 3, C5): `lobsim_calibrate validate`. The estimator is injected (calib::EstimateFn), so
# the library does not link it; the command line does.
add_library(calibvalidate STATIC src/calib/validate.cpp src/calib/validate_report.cpp)
target_include_directories(calibvalidate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(Threads REQUIRED)
target_link_libraries(calibvalidate PUBLIC fingerprint lobster_export sim calibfile Threads::Threads PRIVATE lobsim_options)
add_executable(calib_validate_tests tests/unit/calib_validate_test.cpp)
target_link_libraries(calib_validate_tests PRIVATE calibvalidate calib lobsim_options GTest::gtest_main)
target_compile_definitions(calib_validate_tests PRIVATE LOBSIM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
gtest_discover_tests(calib_validate_tests)
add_test(NAME calibrate_validate_e2e
  COMMAND ${CMAKE_COMMAND}
    -DRUN=$<TARGET_FILE:lobsim_run> -DEXPORT=$<TARGET_FILE:lobsim_export_lobster>
    -DCALIBRATE=$<TARGET_FILE:lobsim_calibrate>
    -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/calib/noise_only_day.json
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/calibrate_validate_e2e
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/calibrate_validate_e2e.cmake)
