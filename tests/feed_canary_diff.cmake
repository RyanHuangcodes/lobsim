# Feed canary (upgrade 6): the seeded differential stream, built with the injected encoder bug, must
# stop at a feed message the rebuilt book refuses: an A published before its executions crosses.
# A failure of any other kind does not count as catching the bug.
#   cmake -DDIFF=<fuzz_diff built with LOBSIM_FEED_BUG> -P feed_canary_diff.cmake
execute_process(COMMAND "${DIFF}" --seed 1 --messages 10000 RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
string(REGEX MATCH "MISMATCH[^\n]*" line "${out}")
message(STATUS "fuzz_diff --seed 1: exit ${rc}\n${line}")
if(rc EQUAL 0)
  message(FATAL_ERROR "the injected feed bug was not caught: fuzz_diff exited 0")
endif()
if(NOT line MATCHES "feed: .*would cross")
  message(FATAL_ERROR "fuzz_diff failed, but not on the feed's crossing check:\n${out}${err}")
endif()
