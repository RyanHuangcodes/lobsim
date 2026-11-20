# Visual replay dashboard (upgrade 4), phase V1: the lobsim_viz_export CLI and its testable core.
# Reached through a single add_subdirectory(viz) in the top-level CMakeLists.txt, placed right
# after enable_testing(), so the tests registered here run under the existing `ctest --test-dir
# build`. Only lob + the orders.log codec (orderlog) + nlohmann_json are used: no simulator (sim)
# and nothing from the upgrade-2 replay (replay).

add_library(viz_core STATIC viz_core.cpp)
target_include_directories(viz_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(viz_core PUBLIC lob orderlog nlohmann_json::nlohmann_json PRIVATE lobsim_options)

add_executable(lobsim_viz_export export_main.cpp)
target_link_libraries(lobsim_viz_export PRIVATE viz_core lobsim_options)

add_executable(viz_tests viz_test.cpp)
target_link_libraries(viz_tests PRIVATE viz_core lobsim_options GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(viz_tests)
