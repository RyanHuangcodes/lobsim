# scripts/run_real_data.sh --calibrate: what it does with the calibrations and with synthetic data, checked with a
# stand-in lobsim_calibrate that only records its command lines and writes a validation.json and a calibration file.
#   * the cross-ticker run uses the calibration file that the pair's own validation.json names (and that pair's median mid),
#     not the first *.json that an earlier run left in the folder;
#   * a missing calibration file is an error (non-zero, a message), not a fallback;
#   * two dates of one ticker are not a cross-ticker check; another ticker name is;
#   * --threads N reaches every validate command;
#   * a SYNTH ticker without a README_SYNTHETIC.txt (hard links, nothing to follow) is refused; with symbolic links to a
#     folder that has one, or for a real ticker, it is not.
#   cmake -DSCRIPT=<run_real_data.sh> -DOUT=<dir> -P run_real_data_guards.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

file(WRITE "${OUT}/stub.sh" [=[#!/bin/bash
echo "$*" >> "$STUB_LOG"
out=""; cal=""; msgs=""
while [ $# -gt 0 ]; do
  case "$1" in --out) out="$2" ;; --calibration) cal="$2" ;; --messages) msgs="$2" ;; esac
  shift
done
base="$(basename "$msgs")"; tk="${base%%_*}"
[ -z "${STUB_FAIL:-}" ] || case "$msgs" in *"$STUB_FAIL"*) exit 1 ;; esac
case "$tk" in TKA) mid=111 ;; TKB) mid=222 ;; *) mid=333 ;; esac
mkdir -p "$out"
name="FRESH_${tk}.json"
if [ -z "${STUB_NOFILE:-}" ] && [ -z "$cal" ]; then echo "{\"median_mid_ticks\": $mid}" > "$out/$name"; fi
echo "{\"calibration\": {\"file\": \"$name\", \"median_mid_ticks\": $mid}}" > "$out/validation.json"
exit 0
]=])
file(CHMOD "${OUT}/stub.sh" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

function(make_pair dir name)
  file(MAKE_DIRECTORY "${dir}")
  file(WRITE "${dir}/${name}_message_5.csv" "x\n")
  file(WRITE "${dir}/${name}_orderbook_5.csv" "x\n")
endfunction()

# run(<case> <data dir> [extra env...]) sets rc, out, err and the log text in ${case}_log
function(run_case case data)
  file(WRITE "${OUT}/${case}.md" "# r\n")
  file(REMOVE "${OUT}/${case}.log")
  execute_process(COMMAND ${CMAKE_COMMAND} -E env STUB_LOG=${OUT}/${case}.log ${ARGN}
                          bash "${SCRIPT}" --calibrate --train 34200-34300 --test 34300-34400 --seeds 2 --threads 3
                          --data-dir "${data}" --report "${OUT}/${case}.md" --out "${OUT}/o_${case}" --bin /nonexistent
                          --calibrate-bin "${OUT}/stub.sh"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(rc "${rc}" PARENT_SCOPE)
  set(err "${out}${err}" PARENT_SCOPE)
  set(log "")
  if(EXISTS "${OUT}/${case}.log")
    file(READ "${OUT}/${case}.log" log)
  endif()
  set(log "${log}" PARENT_SCOPE)
endfunction()

set(D1 2012-06-21_34200000_57600000)
set(D2 2012-06-22_34200000_57600000)

# 1. the cross-ticker run takes the calibration of the run's own validation.json, whatever else is in the folder
make_pair("${OUT}/d1" "TKA_${D1}")
make_pair("${OUT}/d1" "TKB_${D1}")
foreach(t A B)
  file(MAKE_DIRECTORY "${OUT}/o_s1/calibration/TK${t}_${D1}_5")
  file(WRITE "${OUT}/o_s1/calibration/TK${t}_${D1}_5/AAA_stale.json" "{\"median_mid_ticks\": 999}")
endforeach()
run_case(s1 "${OUT}/d1")
string(REGEX MATCHALL "[^\n]*--calibration [^\n]*" cross "${log}")
list(LENGTH cross n)
if(NOT n EQUAL 1)
  message(FATAL_ERROR "expected one cross-ticker call, got ${n}:\n${log}\n${err}")
endif()
string(FIND "${cross}" "AAA_stale" at)
if(NOT at LESS 0)
  message(FATAL_ERROR "the cross-ticker run used a stale calibration file:\n${cross}")
endif()
string(FIND "${cross}" "TKA_${D1}_5/FRESH_TKA.json" at)
if(at LESS 0)
  message(FATAL_ERROR "the cross-ticker run did not use the calibration named by the run's validation.json:\n${cross}")
endif()
string(FIND "${cross}" "--start-price 222" at)
if(at LESS 0)
  message(FATAL_ERROR "the start price is not the median mid of this run's calibration of TKB:\n${cross}")
endif()
string(REGEX MATCHALL "--threads 3" th "${log}")
list(LENGTH th nth)
if(NOT nth EQUAL 3)
  message(FATAL_ERROR "--threads 3 should reach all three validate commands, reached ${nth}:\n${log}")
endif()

# 2. the file the validation.json names is missing: loud failure, no fallback to another file
make_pair("${OUT}/d2" "TKA_${D1}")
make_pair("${OUT}/d2" "TKB_${D1}")
file(MAKE_DIRECTORY "${OUT}/o_s2/calibration/TKA_${D1}_5")
file(WRITE "${OUT}/o_s2/calibration/TKA_${D1}_5/AAA_stale.json" "{\"median_mid_ticks\": 999}")
run_case(s2 "${OUT}/d2" STUB_NOFILE=1)
if(rc EQUAL 0)
  message(FATAL_ERROR "a missing calibration file did not fail the script:\n${err}")
endif()
string(FIND "${err}" "does not exist" at)
if(at LESS 0)
  message(FATAL_ERROR "no message about the missing calibration file:\n${err}")
endif()
string(FIND "${log}" "--calibration" at)
if(NOT at LESS 0)
  message(FATAL_ERROR "the cross-ticker run was started without its calibration file:\n${log}")
endif()

# 3. one ticker on two dates is not a cross-ticker check; a second ticker name is
make_pair("${OUT}/d3" "TKA_${D1}")
make_pair("${OUT}/d3" "TKA_${D2}")
run_case(m7a "${OUT}/d3")
string(FIND "${log}" "--calibration" at)
if(NOT at LESS 0)
  message(FATAL_ERROR "one ticker on two dates was called a cross-ticker check:\n${log}")
endif()
make_pair("${OUT}/d3b" "TKA_${D1}")
make_pair("${OUT}/d3b" "TKA_${D2}")
make_pair("${OUT}/d3b" "TKB_${D1}")
run_case(m7b "${OUT}/d3b")
string(REGEX MATCHALL "[^\n]*--calibration [^\n]*" cross "${log}")
list(LENGTH cross n)
if(NOT n EQUAL 1)
  message(FATAL_ERROR "expected one cross-ticker call (only TKB), got ${n}:\n${log}")
endif()
string(FIND "${cross}" "TKB_${D1}_message" at)
if(at LESS 0)
  message(FATAL_ERROR "the cross-ticker call is not on TKB:\n${cross}")
endif()

# 4. synthetic data without a way to find its README is refused; with a way, or for a real ticker, it is not
make_pair("${OUT}/d4" "SYNTHA_${D1}")
run_case(syn_hard "${OUT}/d4")
if(rc EQUAL 0)
  message(FATAL_ERROR "SYNTH data without README_SYNTHETIC.txt was accepted:\n${err}")
endif()
string(FIND "${err}" "README_SYNTHETIC.txt" at)
if(at LESS 0)
  message(FATAL_ERROR "no message naming README_SYNTHETIC.txt:\n${err}")
endif()
if(EXISTS "${OUT}/syn_hard.log")
  message(FATAL_ERROR "validate was started on unlabelled synthetic data")
endif()
make_pair("${OUT}/store" "SYNTHA_${D1}")
file(WRITE "${OUT}/store/README_SYNTHETIC.txt" "synthetic\n")
file(MAKE_DIRECTORY "${OUT}/d5")
foreach(k message orderbook)
  file(CREATE_LINK "${OUT}/store/SYNTHA_${D1}_${k}_5.csv" "${OUT}/d5/SYNTHA_${D1}_${k}_5.csv" SYMBOLIC)
endforeach()
run_case(syn_link "${OUT}/d5")
if(NOT EXISTS "${OUT}/syn_link.log")
  message(FATAL_ERROR "SYNTH data reached through symbolic links to a folder with the README was refused:\n${err}")
endif()
make_pair("${OUT}/d6" "TKA_${D1}")
run_case(real "${OUT}/d6")
if(NOT EXISTS "${OUT}/real.log")
  message(FATAL_ERROR "a real ticker was refused:\n${err}")
endif()

file(REMOVE_RECURSE "${OUT}")
message(STATUS "run_real_data.sh --calibrate: own calibration, no fallback, ticker names, --threads, synthetic labelling")
