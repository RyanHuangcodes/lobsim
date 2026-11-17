# Command-line level check: lobsim_run writes its files, lobsim_replay replays orders.log,
# and the printed hash equals engine_event_hash in summary.json.
#   cmake -DRUN=<lobsim_run> -DREPLAY=<lobsim_replay> -DCONFIG=<json> -DOUT=<dir> -P cli_replay.cmake
file(REMOVE_RECURSE "${OUT}")

execute_process(COMMAND "${RUN}" --config "${CONFIG}" --duration-s 120 --out "${OUT}"
                RESULT_VARIABLE run_rc OUTPUT_VARIABLE run_out ERROR_VARIABLE run_err)
if(NOT run_rc EQUAL 0)
  message(FATAL_ERROR "lobsim_run failed (${run_rc}): ${run_err}")
endif()

foreach(f orders.log trades.csv bbo.csv mm_fills.csv summary.json)
  if(NOT EXISTS "${OUT}/${f}")
    message(FATAL_ERROR "lobsim_run did not write ${f}")
  endif()
endforeach()

execute_process(COMMAND "${REPLAY}" --log "${OUT}/orders.log"
                RESULT_VARIABLE replay_rc OUTPUT_VARIABLE replay_out ERROR_VARIABLE replay_err)
if(NOT replay_rc EQUAL 0)
  message(FATAL_ERROR "lobsim_replay failed (${replay_rc}): ${replay_err}")
endif()
string(REGEX MATCH "engine event hash +([0-9a-f]+)" _m "${replay_out}")
set(replayed "${CMAKE_MATCH_1}")

file(READ "${OUT}/summary.json" summary)
string(JSON recorded GET "${summary}" engine_event_hash)

if(NOT replayed STREQUAL recorded OR replayed STREQUAL "")
  message(FATAL_ERROR "replayed hash '${replayed}' differs from summary.json '${recorded}'")
endif()

# The tool must also refuse a corrupted log with a non-zero exit code.
file(READ "${OUT}/orders.log" log_text)
file(WRITE "${OUT}/corrupt.log" "${log_text}garbage line\n")
execute_process(COMMAND "${REPLAY}" --log "${OUT}/corrupt.log"
                RESULT_VARIABLE bad_rc OUTPUT_QUIET ERROR_VARIABLE bad_err)
if(bad_rc EQUAL 0)
  message(FATAL_ERROR "lobsim_replay accepted a corrupted log")
endif()
if(NOT bad_err MATCHES "line [0-9]+")
  message(FATAL_ERROR "lobsim_replay's error does not name a line: ${bad_err}")
endif()

file(REMOVE_RECURSE "${OUT}")
message(STATUS "replay hash ${replayed} matches summary.json")
