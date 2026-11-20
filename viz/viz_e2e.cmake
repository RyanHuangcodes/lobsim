# viz_e2e.cmake — end-to-end check for the viz bundle (upgrade 4, phase V3).
#
# Follows the tests/cli_replay.cmake / tests/lobster_closed_loop.cmake pattern: run the real tools
# through a CMake -P script and assert on their output and files.
#
#   cmake -DRUN=<lobsim_run> -DEXPORT=<lobsim_viz_export> -DCONFIG=<json>
#         -DSRC=<source dir> -DOUT=<work dir> -P viz_e2e.cmake
#
# It runs a 5-minute default simulation and the exporter, then checks:
#   * both tools exit 0 and meta.json + data.bin exist;
#   * meta.data_bytes equals the actual data.bin size, and the bundle is within the 8 MB budget;
#   * meta.verified_event_hash equals engine_event_hash from run/summary.json (the hash match);
#   * the required schema keys and series are present (schema validity);
#   * the committed fixture web/test/fixtures/meta.json is NOT stale: its schema_version must equal
#     the freshly exported bundle's schema_version (the code's current kSchemaVersion). If the C++
#     bumps the schema, regenerate the fixture with scripts/make_viz_fixture.sh.
set(budget_bytes 8388608) # 8 MB
file(REMOVE_RECURSE "${OUT}")

# --- run a 5-minute default sim ------------------------------------------------------------------
execute_process(COMMAND "${RUN}" --config "${CONFIG}" --duration-s 300 --out "${OUT}/run"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_run failed (${rc}): ${err}")
endif()
foreach(f orders.log summary.json)
  if(NOT EXISTS "${OUT}/run/${f}")
    message(FATAL_ERROR "lobsim_run did not write ${f}")
  endif()
endforeach()

# --- export the bundle ---------------------------------------------------------------------------
execute_process(COMMAND "${EXPORT}" --run "${OUT}/run" --out "${OUT}/bundle"
                RESULT_VARIABLE rc OUTPUT_VARIABLE export_out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lobsim_viz_export failed (${rc}): ${err}\n${export_out}")
endif()

set(meta_path "${OUT}/bundle/meta.json")
set(data_path "${OUT}/bundle/data.bin")
foreach(f "${meta_path}" "${data_path}")
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "the exporter did not write ${f}")
  endif()
endforeach()

file(READ "${meta_path}" meta)

# --- data.bin size == meta.data_bytes, and within budget -----------------------------------------
file(SIZE "${data_path}" data_size)
string(JSON declared_bytes GET "${meta}" data_bytes)
if(NOT data_size EQUAL declared_bytes)
  message(FATAL_ERROR "data.bin is ${data_size} bytes but meta.data_bytes is ${declared_bytes}")
endif()
if(data_size GREATER budget_bytes)
  message(FATAL_ERROR "bundle data.bin ${data_size} bytes exceeds the ${budget_bytes}-byte budget")
endif()

# --- hash match: meta.verified_event_hash == summary.json engine_event_hash ----------------------
file(READ "${OUT}/run/summary.json" summary)
string(JSON recorded_hash GET "${summary}" engine_event_hash)
string(JSON verified_hash GET "${meta}" verified_event_hash)
if(verified_hash STREQUAL "" OR NOT verified_hash STREQUAL recorded_hash)
  message(FATAL_ERROR "bundle verified_event_hash '${verified_hash}' != summary engine_event_hash '${recorded_hash}'")
endif()

# --- schema validity: required top-level keys ----------------------------------------------------
foreach(key schema_version run_name seed config tick_display verified_event_hash time_range_ns
            sampling ladder_count trade_count heatmap byte_order missing_i32 series data_bytes)
  string(JSON v ERROR_VARIABLE jerr GET "${meta}" ${key})
  if(jerr)
    message(FATAL_ERROR "meta.json is missing required key '${key}': ${jerr}")
  endif()
endforeach()

string(JSON byte_order GET "${meta}" byte_order)
if(NOT byte_order STREQUAL "little")
  message(FATAL_ERROR "meta.byte_order is '${byte_order}', expected 'little'")
endif()
string(JSON missing_i32 GET "${meta}" missing_i32)
if(NOT missing_i32 EQUAL -2147483648)
  message(FATAL_ERROR "meta.missing_i32 is ${missing_i32}, expected -2147483648 (INT32_MIN)")
endif()

# --- schema validity: required series present, each with name/type/offset/count ------------------
string(JSON n_series LENGTH "${meta}" series)
if(n_series LESS 1)
  message(FATAL_ERROR "meta.series is empty")
endif()
set(have_series "")
math(EXPR last "${n_series} - 1")
foreach(i RANGE 0 ${last})
  string(JSON sname GET "${meta}" series ${i} name)
  string(JSON stype GET "${meta}" series ${i} type)
  string(JSON soff  GET "${meta}" series ${i} offset)
  string(JSON scnt  GET "${meta}" series ${i} count)
  if(NOT stype MATCHES "^(f64|i32|u8)$")
    message(FATAL_ERROR "series ${sname} has unknown type '${stype}'")
  endif()
  # offsets of f64/i32 arrays are 8-byte aligned by the writer.
  if(NOT stype STREQUAL "u8")
    math(EXPR rem "${soff} % 8")
    if(NOT rem EQUAL 0)
      message(FATAL_ERROR "series ${sname} offset ${soff} is not 8-byte aligned")
    endif()
  endif()
  list(APPEND have_series "${sname}")
endforeach()

foreach(req ladder_time_ns best_bid best_ask spread mid
            mm_bid mm_ask mm_inventory mm_cash mm_pnl fundamental
            trade_time_ns trade_price trade_qty trade_aggressor heatmap
            bid_px_0 bid_sz_0 ask_px_0 ask_sz_0)
  list(FIND have_series "${req}" idx)
  if(idx EQUAL -1)
    message(FATAL_ERROR "required series '${req}' is absent from the bundle")
  endif()
endforeach()

# --- stale-fixture check: committed fixture schema_version must match the fresh bundle -----------
set(fixture_meta "${SRC}/web/test/fixtures/meta.json")
if(NOT EXISTS "${fixture_meta}")
  message(FATAL_ERROR "committed fixture ${fixture_meta} is missing; run scripts/make_viz_fixture.sh")
endif()
file(READ "${fixture_meta}" fmeta)
string(JSON fixture_schema GET "${fmeta}" schema_version)
string(JSON fresh_schema GET "${meta}" schema_version)
if(NOT fixture_schema EQUAL fresh_schema)
  message(FATAL_ERROR
    "committed fixture schema_version (${fixture_schema}) is stale vs the code's current version "
    "(${fresh_schema}); regenerate it with scripts/make_viz_fixture.sh and commit the result")
endif()

message(STATUS "viz e2e: bundle ${data_size} bytes, hash ${verified_hash} matches summary, "
               "${n_series} series present, fixture schema_version ${fixture_schema} current")
file(REMOVE_RECURSE "${OUT}")
