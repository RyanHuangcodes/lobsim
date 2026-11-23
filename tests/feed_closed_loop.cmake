# Closed loop of the binary feed (upgrade 6): lobsim_run (fixed seed) -> lobsim_feed check, which
# must exit 0; then lobsim_feed encode -> decode, which must agree with the check on every message
# count and on the final book digest, and the file size must equal the fixed message sizes
# (38 A + 33 E + 25 X + 21 D + 14 S bytes). Then the feed goes through MoldUDP64 packets and back,
# byte-identical, and a packet file with one packet left out must report the gap (exit code 2).
#   cmake -DRUN=<lobsim_run> -DFEED=<lobsim_feed> -DCONFIG=<json> -DSEED=<n> -DDURATION=<s> -DOUT=<dir>
#         -P feed_closed_loop.cmake
file(REMOVE_RECURSE "${OUT}")

execute_process(COMMAND "${RUN}" --config "${CONFIG}" --seed ${SEED} --duration-s ${DURATION} --out "${OUT}/run"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_run failed (${rc}): ${err}")
endif()

function(run_feed name)
  execute_process(COMMAND "${FEED}" ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  message(STATUS "lobsim_feed ${ARGN}: exit ${rc}\n${out}${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "lobsim_feed ${ARGV1} exited with ${rc}")
  endif()
  string(REGEX MATCH "feed: [^\n]*" line "${out}")
  if(line STREQUAL "")
    message(FATAL_ERROR "lobsim_feed ${ARGV1} printed no summary line")
  endif()
  set(${name} "${line}" PARENT_SCOPE)
endfunction()

run_feed(check_line check --run "${OUT}/run")
run_feed(encode_line encode --run "${OUT}/run" --out "${OUT}/feed.itch")
run_feed(decode_line decode --feed "${OUT}/feed.itch")

if(NOT check_line STREQUAL encode_line OR NOT check_line STREQUAL decode_line)
  message(FATAL_ERROR "the summary lines differ:\n  check  ${check_line}\n  encode ${encode_line}\n  decode ${decode_line}")
endif()

string(REGEX MATCH "S ([0-9]+) A ([0-9]+) E ([0-9]+) X ([0-9]+) D ([0-9]+) bytes ([0-9]+)" _m "${check_line}")
set(s ${CMAKE_MATCH_1})
set(a ${CMAKE_MATCH_2})
set(e ${CMAKE_MATCH_3})
set(x ${CMAKE_MATCH_4})
set(d ${CMAKE_MATCH_5})
set(bytes ${CMAKE_MATCH_6})
if(NOT s EQUAL 2)
  message(FATAL_ERROR "expected exactly 2 system events, got ${s}")
endif()
if(a EQUAL 0 OR e EQUAL 0 OR d EQUAL 0)
  message(FATAL_ERROR "the run should produce A, E and D messages: ${check_line}")
endif()
math(EXPR want "38 * ${a} + 33 * ${e} + 25 * ${x} + 21 * ${d} + 14 * ${s}")
file(SIZE "${OUT}/feed.itch" size)
if(NOT size EQUAL want OR NOT bytes EQUAL want)
  message(FATAL_ERROR "feed size ${size} (reported ${bytes}), expected 38 A + 33 E + 25 X + 21 D + 14 S = ${want}")
endif()

# MoldUDP64 (F7): packets back to the same feed bytes; a lost packet is a gap (exit code 2).
execute_process(COMMAND "${FEED}" packetize --feed "${OUT}/feed.itch" --out "${OUT}/feed.mold"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message(STATUS "lobsim_feed packetize: exit ${rc}\n${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_feed packetize exited with ${rc}")
endif()
execute_process(COMMAND "${FEED}" depacketize --packets "${OUT}/feed.mold" --out "${OUT}/feed.back.itch"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message(STATUS "lobsim_feed depacketize: exit ${rc}\n${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_feed depacketize exited with ${rc}")
endif()
file(SHA256 "${OUT}/feed.itch" h1)
file(SHA256 "${OUT}/feed.back.itch" h2)
if(NOT h1 STREQUAL h2)
  message(FATAL_ERROR "the feed rebuilt from the MoldUDP64 packets differs from the feed")
endif()
execute_process(COMMAND "${FEED}" packetize --feed "${OUT}/feed.itch" --out "${OUT}/lost.mold" --drop-packet 2
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_feed packetize --drop-packet exited with ${rc}: ${err}")
endif()
execute_process(COMMAND "${FEED}" depacketize --packets "${OUT}/lost.mold" --out "${OUT}/lost.itch"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT out MATCHES "gap +sequence numbers [0-9]+ to [0-9]+")
  message(FATAL_ERROR "a lost packet must be reported as a gap with exit code 2 (got ${rc}):\n${out}${err}")
endif()
message(STATUS "feed closed loop OK: ${check_line}")
