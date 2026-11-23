# Runs `lobsim_feed check --run RUN_DIR` and requires exit code EXPECT (upgrade 6). The feed canary
# build uses it with EXPECT=2: the injected encoder bug must be caught.
#   cmake -DFEED=<lobsim_feed> -DRUN_DIR=<dir with orders.log> -DEXPECT=<code> -P feed_check_exit.cmake
execute_process(COMMAND "${FEED}" check --run "${RUN_DIR}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message(STATUS "lobsim_feed check --run ${RUN_DIR}: exit ${rc}\n${out}${err}")
if(NOT rc EQUAL ${EXPECT})
  message(FATAL_ERROR "lobsim_feed check exited with ${rc}, expected ${EXPECT}")
endif()
