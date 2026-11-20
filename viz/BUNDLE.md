# The lobsim viz bundle format

A viz bundle is what `lobsim_viz_export` writes and what the web dashboard reads. It
is two files in a directory:

- `meta.json` — a small JSON header describing the run and the layout of the binary.
- `data.bin` — a little-endian, 8-byte-aligned, columnar blob of typed arrays.

This document is the authoritative description of schema version **1**. It is derived
from the writer in `viz/viz_core.cpp` (the `write_bundle` function and the `BinWriter`
class) and matches a real 2-minute bundle. If `viz_core.cpp` and this document ever
disagree, `viz_core.cpp` wins and this document is the bug.

## `meta.json`

All keys are always present (schema version 1). Order is not significant.

| Key | Type | Meaning |
|---|---|---|
| `schema_version` | integer | Always `1` for this format. A reader that does not know this value must refuse the bundle with a clear error. |
| `run_name` | string | The run directory name (the `--run` basename). |
| `seed` | integer | The simulation seed (mirrors `config.seed`). |
| `config` | object | The full effective run config. `config.mm` is `null`/absent when the run had no market maker. `config.duration_s` (seconds, float) and `config.tick_display` (price ticks per display unit) are present. |
| `tick_display` | number | Display scale for prices in ticks (e.g. `0.01` means 1 tick = 0.01 price units). Mirrors `config.tick_display`. |
| `verified_event_hash` | string | 16 hex characters: the engine event hash the exporter verified while rebuilding. |
| `time_range_ns` | `[integer, integer]` | `[start_ns, end_ns]`: first and last sample time in nanoseconds. |
| `sampling` | object | `{ladder_dt_ns, heatmap_dt_ns, levels}`: the ladder sample period, the heatmap column period (both ns), and the number of depth levels per side. |
| `ladder_count` | integer | Number of samples in every ladder-axis series (the `count` of those series). |
| `trade_count` | integer | Number of trades (the `count` of every `trade_*` series). |
| `heatmap` | object | See [Heatmap](#heatmap-series) below. |
| `byte_order` | string | Always `"little"`. `data.bin` is little-endian. |
| `missing_i32` | integer | The sentinel for a missing `i32` value: `-2147483648` (`INT32_MIN`). |
| `series` | array | The series table; see [`data.bin`](#databin) below. |
| `data_bytes` | integer | The exact size of `data.bin` in bytes. |

### `heatmap`

| Key | Type | Meaning |
|---|---|---|
| `rows` | integer | Number of price rows. |
| `cols` | integer | Number of time columns. |
| `price_hi` | integer | Highest price tick (maps to row 0). |
| `price_lo` | integer | Lowest price tick (maps to row `rows-1`). |
| `dt_ns` | integer | Column (time bucket) width in nanoseconds. |
| `t0_ns` | integer | Time of column 0 (nanoseconds); equal to `time_range_ns[0]`. |
| `v_max` | number | The reference size used by the log encoding (see below). Legend labels decode to real sizes with this. |
| `pad_ticks` | integer | Padding (in ticks) added above/below the observed price range when choosing `[price_lo, price_hi]`. |
| `clipped_share` | number | Fraction (0..1) of time-weighted resting mass that fell outside `[price_lo, price_hi]` and was clipped. |

## `data.bin`

`data.bin` is a concatenation of columnar arrays, one per `series` entry, in the order
listed in `meta.series`. The host writer is little-endian (x86-64), so every value is
stored little-endian.

Each `series` entry is:

| Field | Type | Meaning |
|---|---|---|
| `name` | string | Series name (see the table below). |
| `type` | string | `"f64"` (8-byte IEEE-754 double), `"i32"` (4-byte signed, two's complement), or `"u8"` (1 byte). |
| `offset` | integer | Byte offset of this array's first element from the start of `data.bin`. |
| `count` | integer | Number of elements. |

**Alignment.** Each array begins on an 8-byte boundary: the writer pads with zero bytes
between arrays so every `offset` is a multiple of 8. A `Float64Array` view can therefore
be created directly over the `ArrayBuffer` at `offset` with no copy. (`i32` and `u8`
arrays are also 8-aligned at their start; only their starts are guaranteed aligned, which
is enough for `Int32Array`/`Uint8Array` views.)

**No parse loop.** A reader fetches `data.bin` as one `ArrayBuffer` and creates one typed
array view per series. There is no per-element parsing.

### Missing values

- `i32`: the value equals `missing_i32` (`-2147483648`, `INT32_MIN`).
- `f64`: the value is `NaN`.

When the run has no market maker (`config.mm` is `null`), every `mm_bid`, `mm_ask`,
`mm_inventory` value is `missing_i32` and every `mm_cash`, `mm_pnl` value is `NaN`. A
reader detects the no-MM case from `config.mm == null` (and may confirm by checking that
the MM series are all-missing) and hides the MM panels.

A one-sided or absent top of book is likewise `missing_i32` (`best_bid`, `best_ask`,
`spread`) or `NaN` (`mid`), and ladder levels that do not exist at a sample are
`missing_i32`.

### Series table

All ladder-axis series have `count == ladder_count` and share the `ladder_time_ns` axis.
All trade series have `count == trade_count` and share the `trade_time_ns` axis.

| Name | Type | Count | Meaning |
|---|---|---|---|
| `ladder_time_ns` | f64 | `ladder_count` | Sample time in ns (sorted ascending). The shared time axis for every ladder series. |
| `best_bid` | i32 | `ladder_count` | Best bid price in ticks (`missing_i32` if none). |
| `best_ask` | i32 | `ladder_count` | Best ask price in ticks (`missing_i32` if none). |
| `spread` | i32 | `ladder_count` | `best_ask - best_bid` in ticks (`missing_i32` if either side missing). |
| `mid` | f64 | `ladder_count` | `(best_bid + best_ask) / 2` in ticks (`NaN` if either side missing). |
| `bid_px_d` (d = 0..9) | i32 | `ladder_count` | Bid price at depth level `d` (0 = top), ticks. |
| `bid_sz_d` (d = 0..9) | i32 | `ladder_count` | Total resting bid size at depth level `d`, lots. |
| `ask_px_d` (d = 0..9) | i32 | `ladder_count` | Ask price at depth level `d`, ticks. |
| `ask_sz_d` (d = 0..9) | i32 | `ladder_count` | Total resting ask size at depth level `d`, lots. |
| `mm_bid` | i32 | `ladder_count` | MM's highest resting buy price, ticks (`missing_i32` if no MM / no quote). |
| `mm_ask` | i32 | `ladder_count` | MM's lowest resting sell price, ticks (`missing_i32` if no MM / no quote). |
| `mm_inventory` | i32 | `ladder_count` | MM net inventory in lots (`missing_i32` if no MM). |
| `mm_cash` | f64 | `ladder_count` | MM cash in ticks (`NaN` if no MM). |
| `mm_pnl` | f64 | `ladder_count` | MM mark-to-mid PnL in ticks: `cash + inventory * mid - fees` (`NaN` if no MM). |
| `fundamental` | f64 | `ladder_count` | Fundamental value at or before the sample time, ticks (`NaN` before the first value). |
| `trade_time_ns` | f64 | `trade_count` | Trade time in ns (sorted ascending). The shared time axis for every trade series. |
| `trade_price` | i32 | `trade_count` | Trade price in ticks. |
| `trade_qty` | i32 | `trade_count` | Trade quantity in lots. |
| `trade_aggressor` | i32 | `trade_count` | Aggressor side: **`0` = BUY, `1` = SELL** (`tsd[i] = aggressor == Buy ? 0 : 1`). Not `1`/`-1`. |
| `heatmap` | u8 | `rows * cols` | Log-encoded resting size per (price, time) cell. See below. |

The ladder-axis `count` is the same for all ladder series (equal to `ladder_count`), and
the trade `count` is the same for all trade series (equal to `trade_count`).

### Heatmap series

`heatmap` is `rows * cols` bytes, stored with the **time column as the outer loop and the
price row as the inner loop**:

```
byte_index(row, col) = col * rows + row
```

- Column `c` (`0 <= c < cols`) is a time bucket: `time_ns(c) = t0_ns + c * dt_ns`.
- Row `r` (`0 <= r < rows`) is a price, counted **down** from the top:
  `price(r) = price_hi - r`. So row 0 is `price_hi` (the highest price) and row
  `rows-1` is `price_lo`.

Each byte is a log-encoded time-weighted mean resting size for that cell.

#### Log encode / decode

Encode a size `v` to a byte code with reference `v_max` (from `meta.heatmap.v_max`):

```
code = (v <= 0)         ? 0
     : clamp(round(255 * ln(1 + v) / ln(1 + v_max)), 1, 255)
```

So `code == 0` means "empty" and codes `1..255` cover positive sizes up to `v_max`.

Decode a byte code back to an approximate real size:

```
size = (code == 0) ? 0
     : expm1(code / 255 * log1p(v_max))      // = e^(code/255 * ln(1+v_max)) - 1
```

Use the decode to label the colour legend in real sizes.

## How to decode in JS

```js
// meta is the parsed meta.json; buf is data.bin as an ArrayBuffer.
if (meta.schema_version !== 1) throw new Error(`unknown schema_version ${meta.schema_version}`);

const views = {};
for (const s of meta.series) {
  if (s.type === "f64")      views[s.name] = new Float64Array(buf, s.offset, s.count);
  else if (s.type === "i32") views[s.name] = new Int32Array(buf, s.offset, s.count);
  else if (s.type === "u8")  views[s.name] = new Uint8Array(buf, s.offset, s.count);
}
// views.ladder_time_ns, views.mid, views.heatmap, ... are now zero-copy typed arrays.
```

No per-element parse loop is needed: each series is a single typed-array view over the
shared `ArrayBuffer`, which is valid because every array starts 8-byte aligned.
