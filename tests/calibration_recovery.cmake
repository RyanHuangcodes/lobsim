# Parameter recovery, CI version (upgrade 3, C3): 20 minutes of the hand-made truth simulated with
# configs/calibrated.json, exported unfiltered at L = 40, estimated, and compared with the truth at
# twice the spec's tolerances. Everything is written into OUT, in the build tree.
#   cmake -DRUN=<lobsim_run> -DEXPORT=<lobsim_export_lobster> -DCALIBRATE=<lobsim_calibrate>
#         -DCONFIG=<configs/calibrated.json> -DTRUTH=<calibration/recovery_truth.json> -DOUT=<dir>
#         -P calibration_recovery.cmake
set(seed 20240101)
set(warmup 300)
set(window 1200)
set(levels 40)
file(REMOVE_RECURSE "${OUT}")
math(EXPR duration "${warmup} + ${window}")
execute_process(COMMAND "${RUN}" --config "${CONFIG}" --seed ${seed} --duration-s ${duration} --out "${OUT}/run"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_run failed (${rc}): ${err}")
endif()
execute_process(COMMAND "${EXPORT}" --run "${OUT}/run" --levels ${levels} --lobster-like --window-start-s ${warmup}
                        --no-filter-window --ticker RECOV --out "${OUT}/files"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_export_lobster failed (${rc}): ${err}")
endif()
set(stem "${OUT}/files/RECOV_2012-06-21_34200000_57600000")
execute_process(COMMAND "${CALIBRATE}" estimate --messages "${stem}_message_${levels}.csv"
                        --orderbook "${stem}_orderbook_${levels}.csv" --out "${OUT}/est"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_calibrate estimate failed (${rc}): ${err}\n${out}")
endif()
file(GLOB estimate "${OUT}/est/RECOV_*.json")
list(LENGTH estimate n)
if(NOT n EQUAL 1)
  message(FATAL_ERROR "expected one calibration file in ${OUT}/est, found ${n}")
endif()
execute_process(COMMAND "${CALIBRATE}" compare --truth "${TRUTH}" --estimate "${estimate}" --tolerance-scale 2
                        --md "${OUT}/compare.md"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message(STATUS "${out}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the recovery comparison failed (exit ${rc}): ${err}")
endif()
# Not vacuous: every rate, both size PMFs and every lifetime median were judged, and at least the
# s = 1 and s = 2 placement buckets of both sides had enough samples to be judged.
if(NOT out MATCHES "passed by group: rate 4, placement ([0-9]+), size 2, lifetime median 4")
  message(FATAL_ERROR "the comparison judged fewer checks than expected")
endif()
if(CMAKE_MATCH_1 LESS 4)
  message(FATAL_ERROR "only ${CMAKE_MATCH_1} placement buckets had enough samples to be judged")
endif()
