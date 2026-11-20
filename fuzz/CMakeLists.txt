# Differential fuzzing: the shared driver, the crash-file decoder.
add_library(fuzz_driver STATIC diff_driver.cpp)
target_include_directories(fuzz_driver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fuzz_driver PUBLIC lob PRIVATE lobsim_options)

add_executable(lobsim_fuzz_decode decode_tool.cpp)
target_link_libraries(lobsim_fuzz_decode PRIVATE fuzz_driver lobsim_options)
