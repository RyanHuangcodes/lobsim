# Calibration file (schema version 1)

A calibration file is one JSON object written by `lobsim_calibrate estimate` (or by hand) and read
by the calibrated background flow. It is read and written by `calib::load`, `calib::save`,
`calib::from_json` and `calib::to_json` (`include/calib/calibration.hpp`). Keys are written in
sorted order with 2-space indentation and a trailing newline, so the same data always gives the
same bytes.

Every key listed below is required, except `report`. Unknown keys are an error. A violation of any
rule makes loading fail with an error that names the field, for example
`placement[sell][3+].counts[4]: must be positive`.

## Units

Prices, placement distances and spreads are in ticks. Sizes are in shares. Times are integer
nanoseconds. Only rates, probabilities, survival values and fitted parameters are floating point.

## `source` labels every number

`source` is one of:

- `real`: measured from a real LOBSTER day;
- `synthetic`: measured from a simulator-generated (fake) day;
- `hand-made`: written by hand (counts and at-risk numbers may be zero or absent).

Every number derived from the file carries this label. Synthetic results are never presented as
real ones.

## Top level

| field | type | rule |
|---|---|---|
| `schema_version` | integer | must be 1 |
| `source` | string | `real`, `synthetic` or `hand-made` |
| `ticker` | string | free text |
| `date` | string | free text, normally `YYYY-MM-DD` |
| `window_from_ns` | integer | nanoseconds after midnight, start of the estimator window (inclusive) |
| `window_to_ns` | integer | end of the window (exclusive); `window_from_ns <= window_to_ns` |
| `observed_ns` | integer | time the rates are divided by; `>= 0` |
| `window_note` | string | how the window was chosen |
| `inputs` | array of `{name, sha256}` | file name only (never a path) and lowercase hex SHA-256 |
| `code_commit` | string | git commit of the estimator, or `"unknown"` |
| `levels` | integer | L of the order-book file; `>= 0` (0 in a hand-made file) |
| `tick_units` | integer | file price units per tick; `>= 1` |
| `median_mid_ticks` | integer | floor of the median mid-price in ticks; `>= 1`; the simulator's start price |
| `rates` | object | see below |
| `placement` | object | see below |
| `placement_fit` | object | `{"buy": fit or null, "sell": fit or null}`, reported only |
| `limit_size` | Pmf | sizes of limit orders, each value `>= 1` |
| `aggressive_size` | Pmf | total size of each aggressive order, each value `>= 1` |
| `pooled_limit_size` | integer `>= 0` | samples moved into the top size bin (sizes above the 99.9th percentile) |
| `pooled_aggressive_size` | integer `>= 0` | the same for aggressive sizes |
| `lifetime` | object | see below |
| `report` | object | optional, defaults to `{}`; free-form, see below |

## Pmf

An empirical distribution on integers: `{"values": [...], "counts": [...]}`. `values` are integers,
strictly increasing. `counts` are positive integers of the same length; `counts[i]` is the number of
samples equal to `values[i]` (a hand-made file may use any positive weights). The sum of the counts
must fit in an unsigned 64-bit integer. An empty Pmf (both arrays empty) means no samples.

## `rates`

`{"limit": {"buy": Rate, "sell": Rate}, "aggressive": {"buy": Rate, "sell": Rate}}`.

A Rate is `{"count": integer >= 0, "per_s": number}`. `per_s` is events per second, finite and
`>= 0`; it is `count / observed seconds` when estimated and stated by hand otherwise (then `count`
may be 0). `limit` counts type-1 submissions by the order's side. `aggressive` counts bursts of
type-4 messages by the side of the aggressor, which is the opposite of the LOBSTER direction (that
one is the resting side).

## `placement`

`{"buy": {"1": Pmf, "2": Pmf, "3+": Pmf}, "sell": {...}}`. The keys are the spread buckets: the
spread s in ticks is 1, 2, or 3 and more. Each Pmf holds placement distances Delta in ticks: the
distance of a new limit order behind the same-side best price (a buy at best bid minus Delta, a
sell at best ask plus Delta). Delta may be negative, inside the spread, but never below -(s - 1):
values are `>= 0` in bucket `1`, `>= -1` in bucket `2`, and unrestricted in bucket `3+`.

`placement_fit[side]` is null or `{"delta0": number, "mu": number, "samples": integer, "method": string}`:
the power-law fit P(Delta) proportional to 1 / (delta0 + Delta)^(1 + mu) for Delta >= 0. It is
reported only and never used by the simulator, which samples the empirical Pmf. `delta0` and `mu`
must be finite.

## `lifetime`

Kaplan-Meier survival of limit-order lifetimes (submission to full cancellation), keyed by the
distance bucket of the order's placement Delta: `"<=0"`, `"1-2"`, `"3-9"`, `">=10"`. Each entry:

| field | type | rule |
|---|---|---|
| `times_ns` | array of integers | the distinct lifetimes at which a cancellation happened; strictly increasing, each `> 0` |
| `survival` | array of numbers | `survival[i]` is S(times_ns[i]); finite, in [0, 1], non-increasing; same length as `times_ns` |
| `at_risk` | array of integers | n_i, orders still at risk just before `times_ns[i]` |
| `events` | array of integers | d_i, cancellations at `times_ns[i]` |
| `orders` | integer | orders that entered this bucket |
| `cancelled` | integer | lifetimes ended by a full cancellation |
| `censored_executed` | integer | orders fully executed first |
| `censored_window_end` | integer | orders still alive at the window end |
| `censored_left_window` | integer | orders priced worse than the published L-th level on their side |

S(t) = 1 before `times_ns[0]`, and S(t) = survival[i] for times_ns[i] <= t < times_ns[i+1]. When the
curve ends above 0 the remaining orders are never cancelled. `at_risk` and `events` are both empty
(a hand-made curve) or both have the length of `times_ns`. An entirely empty entry is valid.

## `report`

Free-form JSON object: diagnostics, counters, excluded messages, sign autocorrelation and the
validation targets. It is written for people and for the validation report, passed through
unchanged by load and save, and never read by the simulator.

## Small complete example

```json
{
  "aggressive_size": {"counts": [4, 1], "values": [1, 3]},
  "code_commit": "unknown",
  "date": "2012-06-21",
  "inputs": [],
  "levels": 0,
  "lifetime": {
    "<=0":  {"at_risk": [], "cancelled": 0, "censored_executed": 0, "censored_left_window": 0,
             "censored_window_end": 0, "events": [], "orders": 0,
             "survival": [0.5, 0.2], "times_ns": [1000000, 5000000]},
    "1-2":  {"at_risk": [], "cancelled": 0, "censored_executed": 0, "censored_left_window": 0,
             "censored_window_end": 0, "events": [], "orders": 0,
             "survival": [0.5, 0.2], "times_ns": [1000000, 5000000]},
    "3-9":  {"at_risk": [], "cancelled": 0, "censored_executed": 0, "censored_left_window": 0,
             "censored_window_end": 0, "events": [], "orders": 0,
             "survival": [0.8, 0.4], "times_ns": [2000000, 9000000]},
    ">=10": {"at_risk": [], "cancelled": 0, "censored_executed": 0, "censored_left_window": 0,
             "censored_window_end": 0, "events": [], "orders": 0,
             "survival": [], "times_ns": []}
  },
  "limit_size": {"counts": [3, 1], "values": [1, 2]},
  "median_mid_ticks": 1000,
  "observed_ns": 60000000000,
  "placement": {
    "buy":  {"1": {"counts": [3, 1], "values": [0, 1]},
             "2": {"counts": [1, 2], "values": [-1, 0]},
             "3+": {"counts": [1, 1], "values": [0, 2]}},
    "sell": {"1": {"counts": [3, 1], "values": [0, 1]},
             "2": {"counts": [1, 2], "values": [-1, 0]},
             "3+": {"counts": [1, 1], "values": [0, 2]}}
  },
  "placement_fit": {"buy": null, "sell": null},
  "pooled_aggressive_size": 0,
  "pooled_limit_size": 0,
  "rates": {
    "aggressive": {"buy": {"count": 0, "per_s": 0.1}, "sell": {"count": 0, "per_s": 0.1}},
    "limit": {"buy": {"count": 0, "per_s": 1.0}, "sell": {"count": 0, "per_s": 1.0}}
  },
  "report": {},
  "schema_version": 1,
  "source": "hand-made",
  "tick_units": 1,
  "ticker": "TOY",
  "window_from_ns": 0,
  "window_note": "hand-made",
  "window_to_ns": 60000000000
}
```
