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

add_library(lob INTERFACE)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lob INTERFACE cxx_std_20)

enable_testing()
include(GoogleTest)

add_executable(lob_tests
  tests/unit/smoke_test.cpp)
target_link_libraries(lob_tests PRIVATE lob lobsim_options GTest::gtest_main)
gtest_discover_tests(lob_tests)
