# Calibrated background flow, closed loop (upgrade 3, C2): a hand-made calibration file drives the
# whole market (no noise traders, no informed trader, no market maker); the run is exported to
# LOBSTER files and replayed in strict mode at compare depth L (every row exact), then in resync mode
# (no correction at any level). The message rates must be near the configured ones.
#   cmake -DRUN=<lobsim_run> -DEXPORT=<lobsim_export_lobster> -DLOBSTER=<lobsim_lobster>
#         -DCONFIG=<json> -DOUT=<dir> -P calibrated_flow_closed_loop.cmake
set(levels 10)
file(REMOVE_RECURSE "${OUT}")
execute_process(COMMAND "${RUN}" --config "${CONFIG}" --out "${OUT}/run"
                RESULT_VARIABLE rc OUTPUT_VARIABLE run_out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_run failed (${rc}): ${err}")
endif()
if(NOT run_out MATCHES "background orders +([0-9]+) limit, ([0-9]+) market")
  message(FATAL_ERROR "lobsim_run did not print the background counters:\n${run_out}")
endif()
set(limits ${CMAKE_MATCH_1})
set(markets ${CMAKE_MATCH_2})
# Configured: 8 limit orders per second and 1 aggressive order per second, for 60 s.
if(limits LESS 380 OR limits GREATER 580 OR markets LESS 30 OR markets GREATER 100)
  message(FATAL_ERROR "message rates off: ${limits} limit orders (expected about 480), ${markets} market orders (about 60)")
endif()

execute_process(COMMAND "${EXPORT}" --run "${OUT}/run" --levels ${levels} --ticker TOY --out "${OUT}/files"
                RESULT_VARIABLE rc OUTPUT_VARIABLE export_out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_export_lobster failed (${rc}): ${err}")
endif()
set(stem TOY_2012-06-21_34200000_57600000)
foreach(mode strict resync)
  set(extra "")
  if(mode STREQUAL "strict")
    set(extra --compare-depth ${levels})
  endif()
  execute_process(COMMAND "${LOBSTER}" --messages "${OUT}/files/${stem}_message_${levels}.csv"
                          --orderbook "${OUT}/files/${stem}_orderbook_${levels}.csv" --mode ${mode} ${extra}
                          --out "${OUT}/replay_${mode}"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE replay_out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "lobsim_lobster --mode ${mode} exited with ${rc}: ${err}\n${replay_out}")
  endif()
endforeach()
file(READ "${OUT}/replay_resync/summary.json" summary)
string(JSON corrected GET "${summary}" resync corrected_by_level)
string(JSON removed GET "${summary}" resync removed_by_rank)
string(JSON corrected1 GET "${summary}" resync corrected_by_level 0)
string(JSON removed1 GET "${summary}" resync removed_by_rank 0)
string(JSON lines GET "${summary}" lines)
if(NOT corrected1 EQUAL 0 OR NOT removed1 EQUAL 0 OR NOT corrected MATCHES "^\\[[0, ]*\\]$" OR NOT removed MATCHES "^\\[[0, ]*\\]$")
  message(FATAL_ERROR "the resync replay corrected the book: corrected ${corrected}, removed ${removed}")
endif()
foreach(name direction_mismatch price_mismatch delete_size_mismatch oversized_reduce unknown_id)
  string(JSON got GET "${summary}" counters ${name})
  if(NOT got EQUAL 0)
    message(FATAL_ERROR "replay counter ${name} is ${got}, expected 0")
  endif()
endforeach()
message(STATUS "calibrated flow closed loop: ${limits} limit and ${markets} market orders, ${lines} message lines, no correction")
