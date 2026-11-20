# run_node_tests.cmake — run the web/test JS suite with node's built-in test runner.
#
# ctest invokes this with:
#   cmake -DNODE=<path-or-empty> -DSRC=<source dir> -P run_node_tests.cmake
#
# When NODE is empty or not an executable the node runtime is absent: this script prints the line
#   viz_js_tests: SKIP node runtime not found
# and exits 0. The test is registered with SKIP_REGULAR_EXPRESSION matching that marker, so ctest
# reports it as "Skipped" rather than passed-silently or failed. A real test failure (node present
# but a suite fails) raises a fatal error instead, so ctest reports a genuine failure.
#
# node 22's `--test` does not accept a bare directory as a positional argument (it treats it as a
# module to load), so the portable spelling that discovers the suite is a glob of the test files.
# All four suites are dependency-free ES modules.

if(NODE STREQUAL "" OR NOT EXISTS "${NODE}")
  message("viz_js_tests: SKIP node runtime not found (NODE='${NODE}')")
  message("viz_js_tests: install Node 18+ on PATH to run web/test; the C++ tests still cover the bundle format")
  return()
endif()

file(GLOB test_files "${SRC}/web/test/*.test.js")
if(test_files STREQUAL "")
  message(FATAL_ERROR "no web/test/*.test.js files found under ${SRC}")
endif()

execute_process(COMMAND "${NODE}" --test ${test_files}
                WORKING_DIRECTORY "${SRC}"
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "node --test web/test failed (exit ${rc})")
endif()
message("node --test web/test: all suites passed")
