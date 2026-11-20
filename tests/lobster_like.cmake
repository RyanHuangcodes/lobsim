# Fake-day mode (upgrade 2b, D3): lobsim_run (fixed seed, 60 s warm-up + 120 s window) ->
# lobsim_export_lobster --lobster-like, twice -> the files must be byte-identical; the notes and the
# injected-counts file must exist; the output guard must refuse data/lobster; lobsim_lobster in
# resync mode must count exactly the injected hidden executions and halt, and see pre-window orders.
#   cmake -DRUN=<lobsim_run> -DEXPORT=<lobsim_export_lobster> -DLOBSTER=<lobsim_lobster>
#         -DCONFIG=<json> -DOUT=<dir> -P lobster_like.cmake
set(levels 10)
file(REMOVE_RECURSE "${OUT}")

execute_process(COMMAND "${RUN}" --config "${CONFIG}" --seed 11 --duration-s 180 --out "${OUT}/run"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_run failed (${rc}): ${err}")
endif()

foreach(copy a b)
  execute_process(COMMAND "${EXPORT}" --run "${OUT}/run" --levels ${levels} --lobster-like
                          --window-start-s 60 --hidden-rate 0.5 --seed 5 --ticker SYNTHT --out "${OUT}/${copy}"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE export_out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "lobsim_export_lobster --lobster-like failed (${rc}): ${err}")
  endif()
endforeach()
message(STATUS "export:\n${export_out}")

set(stem SYNTHT_2012-06-21_34200000_57600000)
foreach(f ${stem}_message_${levels}.csv ${stem}_orderbook_${levels}.csv README_SYNTHETIC.txt SYNTHT_injected.json)
  if(NOT EXISTS "${OUT}/a/${f}")
    message(FATAL_ERROR "the exporter did not write ${f}")
  endif()
  file(SHA256 "${OUT}/a/${f}" ha)
  file(SHA256 "${OUT}/b/${f}" hb)
  if(NOT ha STREQUAL hb)
    message(FATAL_ERROR "${f} differs between two identical runs")
  endif()
endforeach()
file(READ "${OUT}/a/README_SYNTHETIC.txt" notes)
if(NOT notes MATCHES "NOT MARKET DATA")
  message(FATAL_ERROR "README_SYNTHETIC.txt does not say the data is synthetic")
endif()

# The guard: never write synthetic files where real data is expected.
execute_process(COMMAND "${EXPORT}" --run "${OUT}/run" --levels ${levels} --lobster-like
                        --out "${OUT}/guard/data/lobster/"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 1 OR NOT err MATCHES "refusing")
  message(FATAL_ERROR "writing into data/lobster should be refused with exit 1 (got ${rc}): ${err}")
endif()
if(EXISTS "${OUT}/guard")
  message(FATAL_ERROR "the refused export created ${OUT}/guard")
endif()

file(READ "${OUT}/a/SYNTHT_injected.json" injected)
string(JSON want_hidden GET "${injected}" hidden_exec_written)
string(JSON want_halt GET "${injected}" halt_written)
if(want_hidden LESS 1 OR NOT want_halt EQUAL 1)
  message(FATAL_ERROR "injected ${want_hidden} hidden executions and ${want_halt} halts; expected at least 1 and exactly 1")
endif()

execute_process(COMMAND "${LOBSTER}" --messages "${OUT}/a/${stem}_message_${levels}.csv"
                        --orderbook "${OUT}/a/${stem}_orderbook_${levels}.csv" --mode resync --out "${OUT}/replay"
                RESULT_VARIABLE rc OUTPUT_VARIABLE replay_out ERROR_VARIABLE err)
message(STATUS "lobsim_lobster:\n${replay_out}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_lobster exited with ${rc}: ${err}")
endif()
file(READ "${OUT}/replay/summary.json" summary)
string(JSON got_hidden GET "${summary}" counters hidden_exec)
string(JSON got_halt GET "${summary}" counters halt)
string(JSON unknown GET "${summary}" counters unknown_id)
string(JSON seeded GET "${summary}" counters seed_attributed)
if(NOT got_hidden EQUAL want_hidden)
  message(FATAL_ERROR "replay counted ${got_hidden} hidden executions, ${want_hidden} were injected")
endif()
if(NOT got_halt EQUAL 1)
  message(FATAL_ERROR "replay counted ${got_halt} halts, 1 was injected")
endif()
math(EXPR referenced "${unknown} + ${seeded}")
if(referenced LESS 1)
  message(FATAL_ERROR "no message refers to an order from before the window (unknown_id ${unknown}, seed_attributed ${seeded})")
endif()
message(STATUS "fake day: hidden_exec ${got_hidden} = injected, halt ${got_halt}, unknown_id ${unknown}, seed_attributed ${seeded}")

file(REMOVE_RECURSE "${OUT}")
