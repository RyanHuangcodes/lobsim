# lobsim_calibrate validate end to end (upgrade 3, C5) on a short generated LOBSTER-like day (synthetic):
# lobsim_run (180 s) -> lobsim_export_lobster --lobster-like (window from 30 s: file times 34200 to 34350 s)
# -> lobsim_calibrate validate --train 34200-34275 --test 34275-34350 --seeds 2 --no-plots.
#   * validation.md and validation.json exist, validation.md has a row for every scorecard row of
#     validation.json and says the data is synthetic in its title;
#   * the leakage check passed (it ran on the estimator's reported range);
#   * a training period that overlaps the test period, and a test period before the training period, are
#     refused with exit code 1; a missing input file exits 3.
#   cmake -DRUN=<lobsim_run> -DEXPORT=<lobsim_export_lobster> -DCALIBRATE=<lobsim_calibrate>
#         -DCONFIG=<noise-only json> -DOUT=<dir> -P calibrate_validate_e2e.cmake
file(REMOVE_RECURSE "${OUT}")
execute_process(COMMAND "${RUN}" --config "${CONFIG}" --seed 21 --duration-s 180 --out "${OUT}/run"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_run failed (${rc}): ${err}")
endif()
execute_process(COMMAND "${EXPORT}" --run "${OUT}/run" --levels 5 --lobster-like --window-start-s 30 --seed 5
                        --ticker SYNTHE --out "${OUT}/day"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_export_lobster failed (${rc}): ${err}")
endif()
set(stem "${OUT}/day/SYNTHE_2012-06-21_34200000_57600000")
set(base ${CALIBRATE} validate --messages "${stem}_message_5.csv" --orderbook "${stem}_orderbook_5.csv"
         --seeds 2 --warmup-s 20 --resamples 50 --no-plots)

execute_process(COMMAND ${base} --train 34200-34275 --test 34275-34350 --out "${OUT}/val"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_calibrate validate exited with ${rc}:\n${out}\n${err}")
endif()
foreach(f validation.md validation.json)
  if(NOT EXISTS "${OUT}/val/${f}")
    message(FATAL_ERROR "validate did not write ${f}:\n${out}")
  endif()
endforeach()
file(READ "${OUT}/val/validation.md" md)
file(READ "${OUT}/val/validation.json" js)
string(JSON n_rows LENGTH "${js}" scorecard)
if(n_rows LESS 30)
  message(FATAL_ERROR "the scorecard has only ${n_rows} rows")
endif()
math(EXPR last "${n_rows} - 1")
foreach(i RANGE ${last})
  string(JSON name GET "${js}" scorecard ${i} name)
  string(FIND "${md}" "| `${name}` |" at)
  if(at LESS 0)
    message(FATAL_ERROR "validation.md has no row for ${name}")
  endif()
endforeach()
string(FIND "${md}" "(SYNTHETIC data, NOT real market data)" at)
if(at LESS 0)
  message(FATAL_ERROR "validation.md does not say the data is synthetic")
endif()
string(FIND "${md}" "- Leakage check: passed." at)
if(at LESS 0)
  message(FATAL_ERROR "validation.md does not report a passed leakage check")
endif()
string(JSON passed GET "${js}" leakage passed)
if(NOT passed STREQUAL "ON" AND NOT passed STREQUAL "true")
  message(FATAL_ERROR "validation.json: leakage.passed is ${passed}")
endif()
string(FIND "${md}" "Trade-sign memory and volatility clustering are expected misses" at)
if(at LESS 0)
  message(FATAL_ERROR "validation.md lacks the known-gaps paragraph")
endif()
file(GLOB cals "${OUT}/val/SYNTHE_2012-06-21_34200-34275.json")
if(NOT cals)
  message(FATAL_ERROR "the calibration file of the training period was not written beside the report")
endif()

# The period rules are enforced before any work is done.
execute_process(COMMAND ${base} --train 34200-34300 --test 34275-34350 --out "${OUT}/bad1"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 1)
  message(FATAL_ERROR "overlapping --train/--test exited with ${rc}, expected 1:\n${err}")
endif()
execute_process(COMMAND ${base} --train 34275-34350 --test 34200-34275 --out "${OUT}/bad2"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 1)
  message(FATAL_ERROR "out-of-order --train/--test exited with ${rc}, expected 1:\n${err}")
endif()
execute_process(COMMAND ${CALIBRATE} validate --messages "${OUT}/none_message_5.csv" --orderbook "${OUT}/none_orderbook_5.csv"
                        --train 1-5 --test 5-9 --out "${OUT}/bad3"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 3)
  message(FATAL_ERROR "missing input exited with ${rc}, expected 3:\n${err}")
endif()
file(REMOVE_RECURSE "${OUT}")
message(STATUS "validate runs end to end; the leakage check passed; the period rules are enforced")
