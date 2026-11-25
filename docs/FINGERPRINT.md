# The market fingerprint

`include/calib/fingerprint.hpp` computes one scorecard from a LOBSTER message/orderbook pair, for data and for exported simulations alike
(`lobsim_export_lobster --lobster-like --filter-window --price-scale <tick_units>`). `lobsim_fingerprint` prints it as JSON, optionally with
bootstrap intervals (`--bootstrap`) and the halves yardstick (`--halves`). The estimator definitions are in DECISIONS.md D-340 to D-351; the
short version:

| Scalar name | Definition |
|---|---|
| `spread_mean`, `spread_one_tick_share`, `one_sided_share` | time-weighted mean spread in ticks (two-sided time), share of two-sided time at 1 tick, share of all time with one side empty |
| `depth_{buy,sell}_L<k>` | time-weighted mean size at level k, empty level = 0 |
| `vol_1s`, `vol_10s`, `vol_60s` | sample std of log mid returns on a fixed clock |
| `kurtosis_10s` | excess kurtosis (population moments) of the 10 s returns |
| `acf_ret_1s_lag1` | lag-1 autocorrelation of the 1 s returns |
| `acf_abs_10s_lag<k>` | autocorrelation of the absolute 10 s returns, k = 1..10 |
| `sign_acf_lag{1,10,100}` | autocorrelation of burst signs, lag in bursts |
| `fill_le0`, `fill_1_2`, `fill_3_9`, `fill_ge10`, `fill_all` | share of limit orders submitted in the window with a type 4 on their id in the window |
| `executed_share` | executed shares / submitted shares of those orders |
| `lambda_{limit,market}_{buy,sell,total}` | type 1 messages and bursts per second of window |

Non-scalar fields: `spread_pmf` (`calib::Pmf`, ticks, weights in ns), the time accounting (`no_state_ns`, `one_sided_ns`, `empty_ns`, `crossed_ns`),
the counts of what was skipped (`vol[i].skipped`, `placement_unknown`, `off_grid_rows`, `duplicate_order_ids`, `reused_order_ids`).

Autocorrelation: `rho_k = (mean over pairs of c_i c_{i+k}) / (mean of c_i^2)`, c = value minus the mean of the valid values, the pair mean over the
pairs whose two members are valid. Undefined values are NaN in C++ and `null` in JSON.

Use from code: `Prepared::from_files(...)` once per day or run, then `.fingerprint()`, `.bootstrap()`, `.halves()`;
`combine_runs(fingerprints)` for N simulations; `distances(a, b)` for data against simulation.
