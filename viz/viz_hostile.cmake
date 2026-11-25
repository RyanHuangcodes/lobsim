# The viz exporter on hostile run folders (D-909): each must end quickly with exit code 2 and write
# no bundle. A run folder is made from orders.log lines; its summary.json carries the real engine
# hash (from lobsim_replay), so the export gets past the hash check to the parts under test.
#   cmake -DEXPORT=<lobsim_viz_export> -DREPLAY=<lobsim_replay> -DOUT=<dir> -P viz_hostile.cmake
file(REMOVE_RECURSE "${OUT}")

function(hostile name expect_text)
  set(dir "${OUT}/${name}")
  file(MAKE_DIRECTORY "${dir}")
  list(JOIN ARGN "\n" lines)
  file(WRITE "${dir}/orders.log" "# lobsim orders.log v1 max_price=2000000000\n${lines}\n")
  execute_process(COMMAND "${REPLAY}" --log "${dir}/orders.log" OUTPUT_VARIABLE rep RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: lobsim_replay failed")
  endif()
  string(REGEX MATCH "engine event hash +([0-9a-f]+)" _m "${rep}")
  file(WRITE "${dir}/summary.json"
       "{\"seed\":1,\"engine_event_hash\":\"${CMAKE_MATCH_1}\",\"config\":{\"noise_traders\":{\"count\":1}}}\n")
  file(WRITE "${dir}/bbo.csv" "time_ns,bid,bid_qty,ask,ask_qty,mid,fundamental,mm_inventory,mm_pnl\n")
  file(WRITE "${dir}/trades.csv"
       "time_ns,trade_id,price,qty,aggressor_side,maker_order_id,taker_order_id,maker_owner,taker_owner\n")
  execute_process(COMMAND "${EXPORT}" --run "${dir}" --out "${dir}/bundle" RESULT_VARIABLE rc ERROR_VARIABLE err
                  TIMEOUT 30)
  if(NOT rc EQUAL 2 OR NOT err MATCHES "${expect_text}" OR EXISTS "${dir}/bundle/data.bin")
    message(FATAL_ERROR "${name}: expected exit 2 and '${expect_text}' with no bundle, got '${rc}': ${err}")
  endif()
  message(STATUS "${name}: exit 2: ${err}")
endfunction()

# A far-off time: the old exporter sampled the ladder up to it and never finished.
hostile(time_gap "not exactly representable" "0 N 1 1 0 0 0 100 10 0 0" "9000000000000000000 N 1 2 0 0 0 100 10 0 0")
# Prices that span more heatmap rows than the budget has bytes.
hostile(price_span "cannot fit the size budget" "0 N 1 1 0 0 0 1 10 0 0" "1 N 1 2 1 0 0 1999999999 10 0 0")
