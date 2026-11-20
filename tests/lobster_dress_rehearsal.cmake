# Dress rehearsal on a short SYNTHETIC fake day (upgrade 2b, D5; D-212..D-215). Not market data.
# lobsim_run (SYNTHA config, fixed seed, 600 s of simulated time) is exported twice with
# --lobster-like (60 s warm-up, hidden executions and the halt on) and an --audit file:
#   (i)  --no-filter-window: every message from the window start on. The D5 table must hold
#        exactly: no correction above level L (none at level 1), no removed level, 100% agreement
#        over the scored bursts at every trust depth, direction, price and delete-size mismatches
#        and oversized reductions 0, hidden_exec and halt equal to the injected counts, and
#        unknown_id equal to the references to orders that rested below level L when the window
#        opened, made before their price was ever inside the published window (from the audit).
#  (ii)  the full fake day (price-window filter on): hidden_exec and halt equal to the injected
#        counts, direction and price mismatches 0, and tools/lobster_audit.py must reproduce every
#        number of summary.json and trace every discrepancy to a filtered message or to the
#        opening row's depth (exit 0).
#   cmake -DRUN=<lobsim_run> -DEXPORT=<lobsim_export_lobster> -DLOBSTER=<lobsim_lobster>
#         -DPYTHON=<python3> -DAUDIT=<tools/lobster_audit.py> -DCONFIG=<json> -DOUT=<dir>
#         -P lobster_dress_rehearsal.cmake
set(levels 10)
set(stem SYNTHA_2012-06-21_34200000_57600000)
file(REMOVE_RECURSE "${OUT}")

execute_process(COMMAND "${RUN}" --config "${CONFIG}" --seed 20120621 --duration-s 600 --out "${OUT}/run"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_run failed (${rc}): ${err}")
endif()

function(json_get out json)
  string(JSON v ERROR_VARIABLE e GET "${json}" ${ARGN})
  if(e)
    message(FATAL_ERROR "summary.json: ${ARGN}: ${e}")
  endif()
  set(${out} "${v}" PARENT_SCOPE)
endfunction()

# Export, replay in resync mode, run the audit. Sets <variant>_summary, <variant>_audit_json,
# <variant>_hidden, <variant>_halt and <variant>_audit_rc in the caller.
function(rehearse variant)
  execute_process(COMMAND "${EXPORT}" --run "${OUT}/run" --levels ${levels} --lobster-like --window-start-s 60
                          --seed 101 --ticker SYNTHA ${ARGN} --out "${OUT}/${variant}" --audit "${OUT}/${variant}.audit.csv"
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${variant}: lobsim_export_lobster failed (${rc}): ${err}")
  endif()
  set(files --messages "${OUT}/${variant}/${stem}_message_${levels}.csv"
            --orderbook "${OUT}/${variant}/${stem}_orderbook_${levels}.csv")
  execute_process(COMMAND "${LOBSTER}" ${files} --mode resync --out "${OUT}/${variant}/replay"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE replay_out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${variant}: lobsim_lobster exited with ${rc}: ${err}")
  endif()
  message(STATUS "${variant} (SYNTHETIC):\n${replay_out}")
  execute_process(COMMAND "${PYTHON}" "${AUDIT}" --audit "${OUT}/${variant}.audit.csv" ${files}
                          --summary "${OUT}/${variant}/replay/summary.json" --json "${OUT}/${variant}/audit.json"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE audit_out ERROR_VARIABLE err)
  message(STATUS "${variant} audit:\n${audit_out}${err}")
  file(READ "${OUT}/${variant}/replay/summary.json" summary)
  file(READ "${OUT}/${variant}/SYNTHA_injected.json" injected)
  json_get(hidden "${injected}" hidden_exec_written)
  json_get(halt "${injected}" halt_written)
  set(${variant}_summary "${summary}" PARENT_SCOPE)
  set(${variant}_hidden "${hidden}" PARENT_SCOPE)
  set(${variant}_halt "${halt}" PARENT_SCOPE)
  set(${variant}_audit_rc "${rc}" PARENT_SCOPE)
  if(EXISTS "${OUT}/${variant}/audit.json")
    file(READ "${OUT}/${variant}/audit.json" aj)
    set(${variant}_audit_json "${aj}" PARENT_SCOPE)
  endif()
endfunction()

# Both variants: the injected counts, and the mismatches no filtering can explain.
function(check_common variant summary hidden halt)
  json_get(got_hidden "${summary}" counters hidden_exec)
  json_get(got_halt "${summary}" counters halt)
  if(NOT got_hidden EQUAL hidden OR NOT got_halt EQUAL halt OR NOT halt EQUAL 1 OR hidden LESS 1)
    message(FATAL_ERROR "${variant}: hidden_exec ${got_hidden} (injected ${hidden}), halt ${got_halt} (injected ${halt})")
  endif()
  foreach(k direction_mismatch price_mismatch)
    json_get(v "${summary}" counters ${k})
    if(NOT v EQUAL 0)
      message(FATAL_ERROR "${variant}: ${k} is ${v}, expected 0")
    endif()
  endforeach()
endfunction()

# (i) No price-window filter.
rehearse(unfiltered --no-filter-window)
check_common(unfiltered "${unfiltered_summary}" ${unfiltered_hidden} ${unfiltered_halt})
set(s "${unfiltered_summary}")
foreach(k delete_size_mismatch oversized_reduce)
  json_get(v "${s}" counters ${k})
  if(NOT v EQUAL 0)
    message(FATAL_ERROR "unfiltered: ${k} is ${v}, expected 0")
  endif()
endforeach()
math(EXPR last "${levels} - 1")
foreach(i RANGE 0 ${last})
  json_get(corr "${s}" resync corrected_by_level ${i})
  json_get(rem "${s}" resync removed_by_rank ${i})
  if(NOT rem EQUAL 0 OR (i LESS last AND NOT corr EQUAL 0))
    math(EXPR level "${i} + 1")
    message(FATAL_ERROR "unfiltered: level ${level} has ${corr} corrections and ${rem} removals; "
                        "only level ${levels} may be corrected (orders below it when the window opened)")
  endif()
endforeach()
json_get(beyond "${s}" resync removed_beyond_level_l)
if(NOT beyond EQUAL 0)
  message(FATAL_ERROR "unfiltered: ${beyond} levels removed beyond level ${levels}")
endif()
string(JSON n LENGTH "${s}" matching by_trust_depth)
math(EXPR n "${n} - 1")
foreach(i RANGE 0 ${n})
  json_get(t "${s}" matching by_trust_depth ${i} trust_depth)
  json_get(num "${s}" matching by_trust_depth ${i} numerator)
  json_get(den "${s}" matching by_trust_depth ${i} denominator)
  json_get(unk "${s}" matching by_trust_depth ${i} excluded_unknown)
  if(NOT num EQUAL den OR NOT unk EQUAL 0 OR (t EQUAL 3 AND den LESS 1))
    message(FATAL_ERROR "unfiltered: agreement at trust depth ${t} is ${num}/${den} (excluded_unknown ${unk}); "
                        "expected every scored burst to agree, and at least one scored at depth 3")
  endif()
  if(t EQUAL 3)
    set(agreement "${num}/${den}")
  endif()
endforeach()
if(NOT unfiltered_audit_rc EQUAL 0)
  message(FATAL_ERROR "unfiltered: the audit failed (exit ${unfiltered_audit_rc})")
endif()
json_get(unknown "${s}" counters unknown_id)
json_get(deep_refs "${unfiltered_audit_json}" deep_references_before_their_level_was_published)
if(NOT unknown EQUAL deep_refs)
  message(FATAL_ERROR "unfiltered: unknown_id ${unknown}, but the audit counts ${deep_refs} references to orders "
                      "below level ${levels} at the window start before their price was published")
endif()
message(STATUS "unfiltered (SYNTHETIC): level-1 corrections 0, agreement ${agreement} at trust depth 3, "
               "unknown_id ${unknown} = references to unpublished pre-window orders")

# (ii) The full fake day.
rehearse(full)
check_common(full "${full_summary}" ${full_hidden} ${full_halt})
if(NOT full_audit_rc EQUAL 0)
  message(FATAL_ERROR "full: the audit failed (exit ${full_audit_rc}): a number of summary.json was not "
                      "reproduced or a discrepancy has no root in the exporter's ground truth")
endif()
json_get(explained "${full_audit_json}" items corrected_level total)
if(explained LESS 1)
  message(FATAL_ERROR "full: no correction to explain; the price-window filter left no trace")
endif()
message(STATUS "full (SYNTHETIC): the audit reproduced summary.json and explained ${explained} corrected levels")
file(REMOVE_RECURSE "${OUT}")
