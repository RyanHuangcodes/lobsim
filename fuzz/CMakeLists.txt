# Differential fuzzing: the shared drivers, the libFuzzer targets, the crash-file decoder.
add_library(fuzz_driver STATIC diff_driver.cpp replay_parse_driver.cpp)
target_include_directories(fuzz_driver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fuzz_driver PUBLIC lob orderlog PRIVATE lobsim_options)

if(LOBSIM_FUZZ)
  # Instrumented through the global compile options; only this executable links the runtime.
  add_executable(fuzz_diff fuzz_diff.cpp)
  target_link_libraries(fuzz_diff PRIVATE fuzz_driver lobsim_options)
  target_link_options(fuzz_diff PRIVATE -fsanitize=fuzzer,address,undefined)

  add_executable(fuzz_replay_parse fuzz_replay_parse.cpp)
  target_link_libraries(fuzz_replay_parse PRIVATE fuzz_driver lobsim_options)
  target_link_options(fuzz_replay_parse PRIVATE -fsanitize=fuzzer,address,undefined)
else()
  add_executable(lobsim_fuzz_decode decode_tool.cpp)
  target_link_libraries(lobsim_fuzz_decode PRIVATE fuzz_driver lobsim_options)
endif()
