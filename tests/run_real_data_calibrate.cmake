# scripts/run_real_data.sh --calibrate (upgrade 3, C6).
#   1. an empty data folder: exit 3, "Calibration pending data" between the CALIBRATION markers, the REAL-DATA block
#      and the text around the markers untouched, and nothing else written;
#   2. two short generated (synthetic) days in one folder: exit 0; the CALIBRATION block holds a scorecard for each
#      ticker and the cross-ticker table (calibrated on the first, validated on the second at the second's median
#      mid), says the data is synthetic, says the data's span was split in half, and leaves the REAL-DATA block alone.
#   cmake -DSCRIPT=<run_real_data.sh> -DRUN=<lobsim_run> -DEXPORT=<lobsim_export_lobster>
#         -DCALIBRATE=<lobsim_calibrate> -DLOBSTER=<lobsim_lobster> -DCONFIG=<noise-only json> -DOUT=<dir> -P run_real_data_calibrate.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/empty")

function(between report begin end var)
  file(READ "${report}" text)
  string(FIND "${text}" "${begin}" b)
  string(FIND "${text}" "${end}" e)
  if(b LESS 0 OR e LESS b)
    message(FATAL_ERROR "${report} has no ${begin} ... ${end}:\n${text}")
  endif()
  math(EXPR len "${e} - ${b}")
  string(SUBSTRING "${text}" ${b} ${len} inside)
  set(${var} "${inside}" PARENT_SCOPE)
endfunction()

# 1. no data
file(WRITE "${OUT}/empty_report.md" "# report\n\n<!-- REAL-DATA:BEGIN -->\nreal-data text\n<!-- REAL-DATA:END -->\n\nafter\n")
execute_process(COMMAND bash "${SCRIPT}" --calibrate --data-dir "${OUT}/empty" --report "${OUT}/empty_report.md"
                        --out "${OUT}/o1" --bin "${LOBSTER}" --calibrate-bin "${CALIBRATE}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 3)
  message(FATAL_ERROR "run_real_data.sh --calibrate on an empty folder exited with ${rc}, expected 3:\n${out}${err}")
endif()
between("${OUT}/empty_report.md" "<!-- CALIBRATION:BEGIN -->" "<!-- CALIBRATION:END -->" inside)
foreach(want "Calibration pending data" "scripts/run_real_data.sh --calibrate" "data/lobster/" "_message_<L>.csv")
  string(FIND "${inside}" "${want}" at)
  if(at LESS 0)
    message(FATAL_ERROR "the pending block lacks '${want}':\n${inside}")
  endif()
endforeach()
between("${OUT}/empty_report.md" "<!-- REAL-DATA:BEGIN -->" "<!-- REAL-DATA:END -->" real)
string(FIND "${real}" "real-data text" at)
file(READ "${OUT}/empty_report.md" whole)
string(FIND "${whole}" "after" after_at)
if(at LESS 0 OR after_at LESS 0)
  message(FATAL_ERROR "the REAL-DATA block or the text after the markers was changed:\n${whole}")
endif()
if(EXISTS "${OUT}/o1/calibration")
  message(FATAL_ERROR "a calibration run was started on an empty folder")
endif()

# 2. two short synthetic days, one folder
file(MAKE_DIRECTORY "${OUT}/data")
foreach(t A B)
  # Seeds chosen so that every spread bucket is visited in the training half (a bucket the estimator never saw
  # cannot be simulated: the calibration is refused at config load).
  if(t STREQUAL A)
    set(seed 21)
  else()
    set(seed 31)
  endif()
  execute_process(COMMAND "${RUN}" --config "${CONFIG}" --seed ${seed} --duration-s 240 --out "${OUT}/run${t}"
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "lobsim_run failed (${rc}): ${err}")
  endif()
  execute_process(COMMAND "${EXPORT}" --run "${OUT}/run${t}" --levels 5 --lobster-like --window-start-s 30 --seed 5
                          --ticker SYN${t} --out "${OUT}/data"
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "lobsim_export_lobster failed (${rc}): ${err}")
  endif()
endforeach()
# Ticker B's files are symbolic links into another folder: the script must follow them.
file(MAKE_DIRECTORY "${OUT}/store")
file(GLOB b_files "${OUT}/data/SYNB_*")
foreach(f ${b_files})
  get_filename_component(n "${f}" NAME)
  file(RENAME "${f}" "${OUT}/store/${n}")
  file(CREATE_LINK "${OUT}/store/${n}" "${f}" SYMBOLIC)
endforeach()
file(WRITE "${OUT}/report.md" "# report\n\n<!-- REAL-DATA:BEGIN -->\nreal-data text\n<!-- REAL-DATA:END -->\n")
execute_process(COMMAND bash "${SCRIPT}" --calibrate --seeds 2 --data-dir "${OUT}/data" --report "${OUT}/report.md"
                        --out "${OUT}/o2" --bin "${LOBSTER}" --calibrate-bin "${CALIBRATE}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "run_real_data.sh --calibrate on two days exited with ${rc}:\n${out}${err}")
endif()
between("${OUT}/report.md" "<!-- CALIBRATION:BEGIN -->" "<!-- CALIBRATION:END -->" inside)
foreach(want "SYNA_2012-06-21_34200000_57600000_5" "SYNB_2012-06-21_34200000_57600000_5" "Cross-ticker check" "calibrated on SYNA_2012-06-21_34200000_57600000_5, validated on SYNB_2012-06-21_34200000_57600000_5"
             "SYNTHETIC data, NOT real market data" "split in half" "| `spread_mean` |" "| `sign_acf_lag100` |"
             "passed (the estimator read only the training period)")
  string(FIND "${inside}" "${want}" at)
  if(at LESS 0)
    message(FATAL_ERROR "the calibration block lacks '${want}':\n${inside}")
  endif()
endforeach()
string(FIND "${inside}" "Calibration pending data" at)
if(NOT at LESS 0)
  message(FATAL_ERROR "the results block still says calibration is pending")
endif()
between("${OUT}/report.md" "<!-- REAL-DATA:BEGIN -->" "<!-- REAL-DATA:END -->" real)
string(FIND "${real}" "real-data text" at)
if(at LESS 0)
  message(FATAL_ERROR "the REAL-DATA block was changed")
endif()
foreach(d SYNA_2012-06-21_34200000_57600000_5 SYNB_2012-06-21_34200000_57600000_5 cross_SYNA_2012-06-21_34200000_57600000_5_to_SYNB_2012-06-21_34200000_57600000_5)
  if(NOT EXISTS "${OUT}/o2/calibration/${d}/validation.md")
    message(FATAL_ERROR "no validation.md for ${d}")
  endif()
endforeach()
file(REMOVE_RECURSE "${OUT}")
message(STATUS "--calibrate: pending block with exit 3 on no data; scorecards and the cross-ticker table on two days")
