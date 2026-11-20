# Closed loop (upgrade 2, R4): lobsim_run (60 simulated seconds, fixed seed) -> lobsim_export_lobster
# --levels 10 -> lobsim_lobster.
#   VARIANT=unfiltered: every event is exported; strict mode at compare depth 10 must be exact on
#     every row at every depth, with no unknown id, no duplicate id, no mismatch of any kind, and
#     seed attribution only for the references to message 1's order (which the opening seed holds).
#   VARIANT=filtered: the export keeps only messages inside the top-10 window (--filter-window);
#     resync mode must make no correction at level 1.
#   cmake -DRUN=<lobsim_run> -DEXPORT=<lobsim_export_lobster> -DLOBSTER=<lobsim_lobster>
#         -DCONFIG=<json> -DVARIANT=unfiltered|filtered -DOUT=<dir> -P lobster_closed_loop.cmake
set(levels 10)
set(seed 7)
file(REMOVE_RECURSE "${OUT}")

execute_process(COMMAND "${RUN}" --config "${CONFIG}" --seed ${seed} --duration-s 60 --out "${OUT}/run"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_run failed (${rc}): ${err}")
endif()

set(export_args --run "${OUT}/run" --levels ${levels} --out "${OUT}/lobster")
if(VARIANT STREQUAL "filtered")
  list(APPEND export_args --filter-window)
elseif(NOT VARIANT STREQUAL "unfiltered")
  message(FATAL_ERROR "VARIANT must be unfiltered or filtered, not '${VARIANT}'")
endif()
execute_process(COMMAND "${EXPORT}" ${export_args} RESULT_VARIABLE rc OUTPUT_VARIABLE export_out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_export_lobster failed (${rc}): ${err}")
endif()
message(STATUS "export (${VARIANT}):\n${export_out}")
string(REGEX MATCH "messages written +([0-9]+)" _m "${export_out}")
set(written "${CMAKE_MATCH_1}")

set(stem "${OUT}/lobster/SIM_2012-06-21_34200000_57600000")
set(messages "${stem}_message_${levels}.csv")
set(orderbook "${stem}_orderbook_${levels}.csv")
foreach(f "${messages}" "${orderbook}")
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "the exporter did not write ${f}")
  endif()
endforeach()

if(VARIANT STREQUAL "unfiltered")
  set(replay_args --mode strict --compare-depth ${levels})
else()
  set(replay_args --mode resync)
endif()
execute_process(COMMAND "${LOBSTER}" --messages "${messages}" --orderbook "${orderbook}" ${replay_args}
                        --out "${OUT}/replay"
                RESULT_VARIABLE rc OUTPUT_VARIABLE replay_out ERROR_VARIABLE err)
message(STATUS "lobsim_lobster ${replay_args}:\n${replay_out}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_lobster exited with ${rc}: ${err}")
endif()

file(READ "${OUT}/replay/summary.json" summary)
string(JSON lines GET "${summary}" lines)
string(JSON rows GET "${summary}" rows_compared)
if(NOT written MATCHES "^[0-9]+$" OR NOT lines EQUAL written OR NOT rows EQUAL written OR written LESS 1000)
  message(FATAL_ERROR "replayed ${lines} lines (${rows} compared) of ${written} exported messages; a run of 60 s must export at least 1000")
endif()

function(expect_counter name want)
  string(JSON got GET "${summary}" counters ${name})
  if(NOT got EQUAL want)
    message(FATAL_ERROR "counter ${name} is ${got}, expected ${want}")
  endif()
endfunction()

if(VARIANT STREQUAL "unfiltered")
  string(JSON completed GET "${summary}" completed)
  if(NOT completed STREQUAL "ON" AND NOT completed STREQUAL "true")
    message(FATAL_ERROR "strict replay did not complete")
  endif()
  math(EXPR last "${levels} - 1")
  foreach(d RANGE 0 ${last})
    string(JSON exact GET "${summary}" exact_rows ${d} exact)
    if(NOT exact EQUAL rows)
      math(EXPR depth "${d} + 1")
      message(FATAL_ERROR "only ${exact} of ${rows} rows are exact at depth ${depth}")
    endif()
  endforeach()
  foreach(c unknown_id dup_id submit_would_cross submit_bad_price direction_mismatch price_mismatch
            delete_size_mismatch oversized_reduce parse_rejected time_truncated oracle_malformed
            oracle_rejected crossed_state seed_rejected hidden_exec cross_trade halt)
    expect_counter(${c} 0)
  endforeach()
  expect_counter(consumed_by_seed 1)

  # Message 1 is consumed by the seed (spec 4.2): its order lives inside a synthetic seed order, so
  # each later reference to it (types 2-4, until the id is submitted again) is a seed attribution.
  # Counted here from the message file itself.
  file(STRINGS "${messages}" first LIMIT_COUNT 1)
  if(NOT first MATCHES "^[0-9]+\\.[0-9]+,1,([0-9]+),")
    message(FATAL_ERROR "message 1 is not a submission: ${first}")
  endif()
  set(id1 "${CMAKE_MATCH_1}")
  file(STRINGS "${messages}" refs REGEX "^[0-9]+\\.[0-9]+,[1-4],${id1},")
  list(REMOVE_AT refs 0)
  set(seed_refs 0)
  foreach(r IN LISTS refs)
    if(r MATCHES "^[0-9]+\\.[0-9]+,1,")
      break()
    endif()
    math(EXPR seed_refs "${seed_refs} + 1")
  endforeach()
  expect_counter(seed_attributed ${seed_refs})
  message(STATUS "unfiltered closed loop: ${rows} rows exact at every depth 1-${levels}; "
                 "seed_attributed ${seed_refs} = later references to message 1's order ${id1}")
else()
  string(JSON corrected1 GET "${summary}" resync corrected_by_level 0)
  string(JSON removed1 GET "${summary}" resync removed_by_rank 0)
  string(JSON corrected GET "${summary}" resync corrected_by_level)
  string(JSON removed GET "${summary}" resync removed_by_rank)
  if(NOT corrected1 EQUAL 0 OR NOT removed1 EQUAL 0)
    message(FATAL_ERROR "level-1 corrections in the filtered closed loop: corrected ${corrected}, removed ${removed}")
  endif()
  string(REGEX MATCH "messages filtered +([0-9]+)" _m "${export_out}")
  if(NOT CMAKE_MATCH_1 GREATER 0)
    message(FATAL_ERROR "the filtered export dropped no message, so it does not test the window")
  endif()
  message(STATUS "filtered closed loop: no level-1 correction; corrected by level ${corrected}, removed by rank ${removed}")
endif()

file(REMOVE_RECURSE "${OUT}")
