# The exporter on a hand-written orders.log (tests/fixtures/lobster_export/): every mapping rule of
# lobsim_export_lobster on one small log (priority-kept and priority-losing modifies, a modify that
# crosses, self-trade prevention of both kinds, IOC, FOK and market remainders, a rejected request).
# The expected files were written by hand from the rules, not by the exporter.
#   1. --levels 3: both files must equal expected_*_3.csv; lobsim_lobster --mode strict
#      --compare-depth 3 on them must exit 0.
#   2. --levels 1 --filter-window: both files must equal filtered_*_1.csv.
#   cmake -DEXPORT=<lobsim_export_lobster> -DLOBSTER=<lobsim_lobster> -DFIXTURE=<dir> -DOUT=<dir>
#         -P lobster_export_fixture.cmake
set(stem "SIM_2012-06-21_34200000_57600000")
file(REMOVE_RECURSE "${OUT}")

function(export_and_compare levels expected_prefix)
  execute_process(COMMAND "${EXPORT}" --run "${FIXTURE}" --levels ${levels} ${ARGN} --out "${OUT}/l${levels}"
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "lobsim_export_lobster --levels ${levels} ${ARGN} failed (${rc}): ${err}")
  endif()
  foreach(kind message orderbook)
    set(got "${OUT}/l${levels}/${stem}_${kind}_${levels}.csv")
    set(want "${FIXTURE}/${expected_prefix}_${stem}_${kind}_${levels}.csv")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${got}" "${want}" RESULT_VARIABLE diff)
    if(NOT diff EQUAL 0)
      file(READ "${got}" text)
      message(FATAL_ERROR "${got} differs from ${want}; it is:\n${text}")
    endif()
  endforeach()
endfunction()

export_and_compare(3 expected)
execute_process(COMMAND "${LOBSTER}" --messages "${OUT}/l3/${stem}_message_3.csv"
                        --orderbook "${OUT}/l3/${stem}_orderbook_3.csv" --mode strict --compare-depth 3
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "strict replay of the exported fixture exited with ${rc}:\n${out}${err}")
endif()

export_and_compare(1 filtered --filter-window)
file(REMOVE_RECURSE "${OUT}")
message(STATUS "exporter output equals the hand-written files; strict replay exact")
