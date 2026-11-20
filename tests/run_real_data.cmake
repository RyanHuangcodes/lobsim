# scripts/run_real_data.sh and the "data missing" behaviour of lobsim_lobster (upgrade 2b).
#   1. lobsim_lobster on two missing files exits 3 and says which names to get and where to put them.
#   2. the script on an empty data folder exits 3 and writes "Data not provided yet" between the markers.
#   3. the script on a copy of a hand-written fixture pair exits 0 and writes numbers between the markers
#      (those numbers are the fixture's, not a market's).
#   cmake -DSCRIPT=<run_real_data.sh> -DLOBSTER=<lobsim_lobster> -DFIXTURE=<fixture dir> -DOUT=<dir>
#         -P run_real_data.cmake
set(stem "TEST_2012-06-21_34200000_57600000")
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/empty")

function(between_markers report var)
  file(READ "${report}" text)
  string(FIND "${text}" "<!-- REAL-DATA:BEGIN -->" b)
  string(FIND "${text}" "<!-- REAL-DATA:END -->" e)
  if(b LESS 0 OR e LESS b)
    message(FATAL_ERROR "${report} has no REAL-DATA markers:\n${text}")
  endif()
  string(SUBSTRING "${text}" ${b} ${e} inside)
  set(${var} "${inside}" PARENT_SCOPE)
endfunction()

# 1. missing files
execute_process(COMMAND "${LOBSTER}" --messages "${OUT}/empty/${stem}_message_10.csv"
                        --orderbook "${OUT}/empty/${stem}_orderbook_10.csv"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 3)
  message(FATAL_ERROR "lobsim_lobster on missing files exited with ${rc}, expected 3:\n${out}${err}")
endif()
foreach(want "${stem}_message_10.csv" "${stem}_orderbook_10.csv" "data/lobster/")
  string(FIND "${err}" "${want}" at)
  if(at LESS 0)
    message(FATAL_ERROR "the missing-file message lacks '${want}':\n${err}")
  endif()
endforeach()

# 2. script, no data
file(WRITE "${OUT}/empty_report.md" "# report\n\n<!-- REAL-DATA:BEGIN -->\nold text\n<!-- REAL-DATA:END -->\n\nafter\n")
execute_process(COMMAND bash "${SCRIPT}" --data-dir "${OUT}/empty" --report "${OUT}/empty_report.md"
                        --out "${OUT}/o1" --bin "${LOBSTER}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 3)
  message(FATAL_ERROR "run_real_data.sh on an empty folder exited with ${rc}, expected 3:\n${out}${err}")
endif()
between_markers("${OUT}/empty_report.md" inside)
string(FIND "${inside}" "Data not provided yet" at)
string(FIND "${inside}" "old text" old)
if(at LESS 0 OR NOT old LESS 0)
  message(FATAL_ERROR "the block between the markers is wrong:\n${inside}")
endif()
file(READ "${OUT}/empty_report.md" whole)
string(FIND "${whole}" "after" after_at)
if(after_at LESS 0)
  message(FATAL_ERROR "text after the END marker was lost:\n${whole}")
endif()

# 3. script, one fixture pair
file(MAKE_DIRECTORY "${OUT}/data")
foreach(kind message orderbook)
  file(COPY_FILE "${FIXTURE}/${stem}_${kind}_3.csv" "${OUT}/data/${stem}_${kind}_3.csv")
endforeach()
file(WRITE "${OUT}/pair_report.md" "# report without markers\n")
execute_process(COMMAND bash "${SCRIPT}" --data-dir "${OUT}/data" --report "${OUT}/pair_report.md"
                        --out "${OUT}/o2" --bin "${LOBSTER}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "run_real_data.sh on a fixture pair exited with ${rc}:\n${out}${err}")
endif()
between_markers("${OUT}/pair_report.md" inside)
foreach(want "${stem}_3" "27 of 27" "28.57" "| resync |")
  string(FIND "${inside}" "${want}" at)
  if(at LESS 0)
    message(FATAL_ERROR "the results block lacks '${want}':\n${inside}")
  endif()
endforeach()
string(FIND "${inside}" "Data not provided yet" at)
if(NOT at LESS 0)
  message(FATAL_ERROR "the results block still says data is not provided:\n${inside}")
endif()
foreach(mode strict report resync)
  if(NOT EXISTS "${OUT}/o2/${stem}_3/${mode}/summary.json")
    message(FATAL_ERROR "no summary.json kept for ${mode}")
  endif()
endforeach()
file(REMOVE_RECURSE "${OUT}")
message(STATUS "missing data exits 3 with the expected names; the script writes both blocks")
