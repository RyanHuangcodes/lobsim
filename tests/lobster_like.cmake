# Fake-day mode (upgrade 2b, D3): lobsim_run (fixed seed, 60 s warm-up + 120 s window) ->
# lobsim_export_lobster --lobster-like, twice -> the files must be byte-identical; the notes and the
# injected-counts file must exist; the output guard must refuse data/lobster; lobsim_lobster in
# resync mode must count exactly the injected hidden executions and halt, and see pre-window orders.
# The --audit file must account for every message (written, filtered, before the window, injected)
# without changing the exported files, and --no-filter-window / --no-halt / --hidden-rate 0 must
# switch their quirk off.
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

# The isolation options (--no-filter-window, --no-halt, --hidden-rate 0) and the --audit ground truth.
function(audit_counts file prefix)
  file(STRINGS "${file}" rows)
  set(written 0)
  set(filtered 0)
  set(before 0)
  set(injected 0)
  foreach(r IN LISTS rows)
    if(r MATCHES ",written,[0-9]+$")
      math(EXPR written "${written} + 1")
    elseif(r MATCHES ",filtered,[0-9]+$")
      math(EXPR filtered "${filtered} + 1")
    elseif(r MATCHES ",before_window,[0-9]+$")
      math(EXPR before "${before} + 1")
    elseif(r MATCHES ",injected,[0-9]+$")
      math(EXPR injected "${injected} + 1")
    endif()
  endforeach()
  set(${prefix}_written ${written} PARENT_SCOPE)
  set(${prefix}_filtered ${filtered} PARENT_SCOPE)
  set(${prefix}_before ${before} PARENT_SCOPE)
  set(${prefix}_injected ${injected} PARENT_SCOPE)
endfunction()

foreach(variant full isolated)
  if(variant STREQUAL "full")
    set(extra --hidden-rate 0.5)
  else()
    set(extra --no-filter-window --no-halt --hidden-rate 0)
  endif()
  execute_process(COMMAND "${EXPORT}" --run "${OUT}/run" --levels ${levels} --lobster-like --window-start-s 60
                          --seed 5 ${extra} --ticker SYNTHT --out "${OUT}/${variant}" --audit "${OUT}/${variant}.audit.csv"
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "lobsim_export_lobster --lobster-like ${extra} --audit failed (${rc}): ${err}")
  endif()
  file(STRINGS "${OUT}/${variant}/${stem}_message_${levels}.csv" lines)
  list(LENGTH lines n_lines)
  file(READ "${OUT}/${variant}/SYNTHT_injected.json" inj)
  string(JSON n_filtered GET "${inj}" messages_filtered)
  string(JSON n_before GET "${inj}" messages_before_window)
  string(JSON n_hidden GET "${inj}" hidden_exec_written)
  string(JSON n_halt GET "${inj}" halt_written)
  audit_counts("${OUT}/${variant}.audit.csv" a)
  math(EXPR a_lines "${a_written} + ${a_injected}")
  math(EXPR want_injected "${n_hidden} + ${n_halt}")
  if(NOT a_lines EQUAL n_lines OR NOT a_filtered EQUAL n_filtered OR NOT a_before EQUAL n_before
     OR NOT a_injected EQUAL want_injected)
    message(FATAL_ERROR "${variant}: the audit (written ${a_written}, injected ${a_injected}, filtered ${a_filtered}, "
                        "before the window ${a_before}) does not match the files (${n_lines} lines) and the injected "
                        "counts (filtered ${n_filtered}, before the window ${n_before}, hidden ${n_hidden}, halt ${n_halt})")
  endif()
  if(variant STREQUAL "full")
    foreach(f ${stem}_message_${levels}.csv ${stem}_orderbook_${levels}.csv)
      file(SHA256 "${OUT}/a/${f}" h_plain)
      file(SHA256 "${OUT}/full/${f}" h_audit)
      if(NOT h_plain STREQUAL h_audit)
        message(FATAL_ERROR "--audit changed ${f}")
      endif()
    endforeach()
    if(a_filtered LESS 1)
      message(FATAL_ERROR "full: nothing was filtered")
    endif()
  else()
    if(NOT a_filtered EQUAL 0 OR NOT n_hidden EQUAL 0 OR NOT n_halt EQUAL 0)
      message(FATAL_ERROR "isolated: filtered ${a_filtered}, hidden ${n_hidden}, halt ${n_halt}; all must be 0")
    endif()
  endif()
  message(STATUS "${variant}: ${n_lines} lines; audit written ${a_written}, injected ${a_injected}, filtered ${a_filtered}, before the window ${a_before}")
endforeach()

# Contradictory or misplaced isolation options are usage errors (exit 1).
foreach(args "--no-halt" "--no-filter-window" "--lobster-like;--no-halt;--halt-at-s;90"
             "--lobster-like;--filter-window;--no-filter-window")
  execute_process(COMMAND "${EXPORT}" --run "${OUT}/run" --levels ${levels} ${args} --out "${OUT}/usage"
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
  if(NOT rc EQUAL 1)
    message(FATAL_ERROR "lobsim_export_lobster ${args} should be refused with exit 1 (got ${rc})")
  endif()
endforeach()

file(REMOVE_RECURSE "${OUT}")
