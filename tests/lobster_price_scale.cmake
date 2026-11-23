# The exporter's --price-scale N (upgrade 3, C4) on the hand-written orders.log of
# tests/fixtures/lobster_export/: for the plain export, the window-filtered export and the
# --lobster-like export, the files written with --price-scale 100 must be the files written without it
# with every real price multiplied by 100 (message price, orderbook level prices), and nothing else
# changed. The empty-level placeholders (9999999999, -9999999999, and 0 under --lobster-like) and the
# halt's price -1 stay as they are. A scale whose largest price would reach 9999999999 is refused.
#   cmake -DEXPORT=<lobsim_export_lobster> -DFIXTURE=<dir> -DOUT=<dir> -P lobster_price_scale.cmake
set(stem "SIM_2012-06-21_34200000_57600000")
file(REMOVE_RECURSE "${OUT}")
set(scale 100)

# Scales one message file's text (type 7 keeps its price) or one orderbook file's text.
function(scale_text kind text out_var)
  string(REGEX REPLACE "\n$" "" text "${text}")
  string(REPLACE "\n" ";" lines "${text}")
  set(result "")
  foreach(line IN LISTS lines)
    string(REPLACE "," ";" f "${line}")
    list(LENGTH f n)
    set(out_fields "")
    set(i 0)
    foreach(v IN LISTS f)
      set(nv "${v}")
      if(kind STREQUAL "message")
        list(GET f 1 type)
        if(i EQUAL 4 AND NOT type EQUAL 7)
          math(EXPR nv "${v} * ${scale}")
        endif()
      else()
        math(EXPR col "${i} % 4")
        if((col EQUAL 0 OR col EQUAL 2) AND NOT v STREQUAL "9999999999" AND NOT v STREQUAL "-9999999999")
          math(EXPR nv "${v} * ${scale}")
        endif()
      endif()
      list(APPEND out_fields "${nv}")
      math(EXPR i "${i} + 1")
    endforeach()
    list(JOIN out_fields "," joined)
    string(APPEND result "${joined}\n")
  endforeach()
  set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

function(check_variant name levels)
  foreach(s 1 ${scale})
    execute_process(COMMAND "${EXPORT}" --run "${FIXTURE}" --levels ${levels} ${ARGN} --price-scale ${s}
                            --ticker SIM --out "${OUT}/${name}_${s}"
                    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "${name}: lobsim_export_lobster --price-scale ${s} failed (${rc}): ${err}")
    endif()
  endforeach()
  foreach(kind message orderbook)
    set(file "${stem}_${kind}_${levels}.csv")
    file(READ "${OUT}/${name}_1/${file}" base)
    file(READ "${OUT}/${name}_${scale}/${file}" scaled)
    scale_text(${kind} "${base}" want)
    if(NOT scaled STREQUAL want)
      message(FATAL_ERROR "${name}: ${kind} file with --price-scale ${scale} is not the unscaled file with prices x ${scale}\nwanted:\n${want}\ngot:\n${scaled}")
    endif()
    if(base STREQUAL scaled)
      message(FATAL_ERROR "${name}: the ${kind} file did not change under --price-scale ${scale}")
    endif()
  endforeach()
endfunction()

check_variant(plain 3)
check_variant(filtered 1 --filter-window)
check_variant(like 3 --lobster-like --window-start-s 0 --hidden-rate 1000000 --seed 3)

# The default (no option) is the same as --price-scale 1.
execute_process(COMMAND "${EXPORT}" --run "${FIXTURE}" --levels 3 --ticker SIM --out "${OUT}/default"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "default export failed: ${err}")
endif()
foreach(kind message orderbook)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}/default/${stem}_${kind}_3.csv"
                          "${OUT}/plain_1/${stem}_${kind}_3.csv" RESULT_VARIABLE diff)
  if(NOT diff EQUAL 0)
    message(FATAL_ERROR "${kind}: no option differs from --price-scale 1")
  endif()
endforeach()

# Placeholders and the halt really are in the --lobster-like output being checked.
file(READ "${OUT}/like_1/${stem}_message_3.csv" like_msgs)
if(NOT like_msgs MATCHES ",7,0,0,-1,1")
  message(FATAL_ERROR "the --lobster-like check has no halt message:\n${like_msgs}")
endif()
file(READ "${OUT}/plain_100/${stem}_orderbook_3.csv" plain_book)
if(NOT plain_book MATCHES "9999999999,0" OR NOT plain_book MATCHES "-9999999999,0")
  message(FATAL_ERROR "the plain check has no empty-level placeholders:\n${plain_book}")
endif()

# Refused scales: not positive, not a number, and one that would reach the placeholder price
# (the fixture's largest price is 1000000, so 9999 is the largest scale allowed).
foreach(bad 0 -3 abc 10000 99999999999)
  execute_process(COMMAND "${EXPORT}" --run "${FIXTURE}" --levels 3 --price-scale ${bad} --out "${OUT}/bad"
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 1)
    message(FATAL_ERROR "--price-scale ${bad} should be refused with exit 1 (got ${rc})")
  endif()
endforeach()
execute_process(COMMAND "${EXPORT}" --run "${FIXTURE}" --levels 3 --price-scale 9999 --out "${OUT}/max"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "--price-scale 9999 should be accepted (${rc}): ${err}")
endif()

file(REMOVE_RECURSE "${OUT}")
message(STATUS "--price-scale ${scale}: prices x ${scale}, everything else identical")
