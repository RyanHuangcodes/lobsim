# Replay canary check (upgrade 2, R5): runs lobsim_lobster in strict mode, comparing every level of
# the orderbook file, and passes only if the replay FAILS the way a strict failure does: exit code 2.
# A launch failure, a usage error or an input error (exit 1) is not a detection, and neither is a
# clean replay (exit 0). Prints the strict-mode row index of the first non-exact row ("messages to
# detection"). Either a FIXTURE directory or MESSAGES and ORDERBOOK files:
#   cmake -DLOBSTER=<lobsim_lobster> (-DFIXTURE=<dir> | -DMESSAGES=<file> -DORDERBOOK=<file>)
#         -DLABEL=<text> -DOUT=<dir> -P replay_canary_check.cmake
if(DEFINED FIXTURE)
  file(GLOB messages "${FIXTURE}/*_message_*.csv")
  file(GLOB orderbook "${FIXTURE}/*_orderbook_*.csv")
else()
  set(messages "${MESSAGES}")
  set(orderbook "${ORDERBOOK}")
endif()
list(LENGTH messages n_messages)
list(LENGTH orderbook n_orderbook)
if(NOT n_messages EQUAL 1 OR NOT n_orderbook EQUAL 1)
  message(FATAL_ERROR "${LABEL}: expected one message and one orderbook file, found ${n_messages} and ${n_orderbook}")
endif()
if(NOT orderbook MATCHES "_orderbook_([0-9]+)\\.csv$")
  message(FATAL_ERROR "${LABEL}: cannot read the level count from ${orderbook}")
endif()
set(levels "${CMAKE_MATCH_1}")

file(REMOVE_RECURSE "${OUT}")
execute_process(COMMAND "${LOBSTER}" --messages "${messages}" --orderbook "${orderbook}"
                        --mode strict --compare-depth ${levels} --out "${OUT}"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc STREQUAL "2")
  message(FATAL_ERROR "${LABEL}: NOT CAUGHT, lobsim_lobster exited with '${rc}', expected 2 (strict failure). ${err}")
endif()
file(READ "${OUT}/summary.json" summary)
string(JSON line GET "${summary}" strict_failure_line)
message(STATUS "${LABEL}: CAUGHT, first non-exact row (messages to detection): ${line}")
file(REMOVE_RECURSE "${OUT}")
