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

# ---------------------------------------------------------------------------------------
# Phase V3 tests.
# ---------------------------------------------------------------------------------------

# End-to-end: a 5-minute default sim + export, checked for size, hash match and schema validity,
# plus a stale-fixture guard (fails if web/test/fixtures/meta.json schema_version != the code's).
# lobsim_run is defined in the top-level CMakeLists; lobsim_viz_export is defined above.
add_test(NAME viz_export_e2e
         COMMAND ${CMAKE_COMMAND}
                 -DRUN=$<TARGET_FILE:lobsim_run>
                 -DEXPORT=$<TARGET_FILE:lobsim_viz_export>
                 -DCONFIG=${CMAKE_SOURCE_DIR}/configs/default.json
                 -DSRC=${CMAKE_SOURCE_DIR}
                 -DOUT=${CMAKE_CURRENT_BINARY_DIR}/viz_e2e
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/viz_e2e.cmake)
# The 5-minute sim can take a minute or two; give it headroom.
set_tests_properties(viz_export_e2e PROPERTIES TIMEOUT 600)

# Hostile run folders: a far-off time and an extreme price span must fail fast with exit code 2.
add_test(NAME viz_export_hostile
         COMMAND ${CMAKE_COMMAND}
                 -DEXPORT=$<TARGET_FILE:lobsim_viz_export>
                 -DREPLAY=$<TARGET_FILE:lobsim_replay>
                 -DOUT=${CMAKE_CURRENT_BINARY_DIR}/viz_hostile
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/viz_hostile.cmake)
set_tests_properties(viz_export_hostile PROPERTIES TIMEOUT 120)

# JS suite (node --test web/test). Skips cleanly when node is absent: find_program resolves node at
# configure time, the wrapper prints a SKIP marker and exits 0 when it is missing, and ctest treats
# that marker (via SKIP_REGULAR_EXPRESSION) as a skip rather than a pass or a failure.
find_program(NODE_EXECUTABLE node)
add_test(NAME viz_js_tests
         COMMAND ${CMAKE_COMMAND}
                 -DNODE=${NODE_EXECUTABLE}
                 -DSRC=${CMAKE_SOURCE_DIR}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_node_tests.cmake)
set_tests_properties(viz_js_tests PROPERTIES SKIP_REGULAR_EXPRESSION "viz_js_tests: SKIP")
if(NOT NODE_EXECUTABLE)
  message(STATUS "viz: node not found at configure time; viz_js_tests will report SKIPPED")
endif()
