// bundle.js — decode a lobsim viz bundle (meta.json + data.bin).
//
// Pure ES module: no DOM, no fetch, importable by node for unit tests.
// See viz/BUNDLE.md for the authoritative format (schema version 1).

export const SCHEMA_VERSION = 1;

// The sentinel for a missing i32 value (INT32_MIN), per meta.missing_i32.
export const MISSING_I32 = -2147483648;

/** True when an i32 value is the missing sentinel. */
export function isMissingI32(v) {
  return v === MISSING_I32;
}

/**
 * Decode a heatmap u8 code back to an approximate real size.
 * code 0 => 0; code 1..255 => expm1(code/255 * log1p(vMax)).
 * Inverse of the exporter encode: code = clamp(round(255*ln(1+v)/ln(1+vMax)), 1, 255).
 */
export function decodeLogCode(code, vMax) {
  if (code <= 0) return 0;
  if (!(vMax > 0)) return 0;
  return Math.expm1((code / 255) * Math.log1p(vMax));
}

/** Byte index of heatmap cell (row, col): stored time-column outer, price-row inner. */
export function heatmapIndex(row, col, rows) {
  return col * rows + row;
}

/** Price (ticks) of heatmap row r: row 0 = price_hi, price decreases downward. */
export function rowToPrice(row, priceHi) {
  return priceHi - row;
}

/** Time (ns) of heatmap column c. */
export function colToTimeNs(col, t0Ns, dtNs) {
  return t0Ns + col * dtNs;
}

/**
 * Does this run have a market maker?
 * True when config.mm is present/non-null AND the mm series are not all-missing.
 * `views` is optional; when omitted, only config.mm is consulted.
 */
export function hasMM(meta, views) {
  const cfg = meta && meta.config;
  const cfgHasMM = !!(cfg && cfg.mm !== null && cfg.mm !== undefined);
  if (!cfgHasMM) return false;
  if (!views || !views.mm_inventory) return true;
  const inv = views.mm_inventory;
  for (let i = 0; i < inv.length; i++) {
    if (!isMissingI32(inv[i])) return true;
  }
  return false;
}

const TYPE_VIEWS = {
  f64: Float64Array,
  i32: Int32Array,
  u8: Uint8Array,
};

/**
 * Decode a bundle into typed-array views over `arrayBuffer`.
 * No per-element parse loop: one zero-copy typed array per series.
 *
 * @param {object} metaObj parsed meta.json
 * @param {ArrayBuffer} arrayBuffer contents of data.bin
 * @returns {{meta: object, views: Object<string,TypedArray>, series: Array}}
 */
export function loadBundle(metaObj, arrayBuffer) {
  if (!metaObj || typeof metaObj !== "object") {
    throw new Error("bundle: meta.json is missing or not an object");
  }
  if (metaObj.schema_version !== SCHEMA_VERSION) {
    throw new Error(
      `bundle: unknown schema_version ${metaObj.schema_version} (this dashboard supports ${SCHEMA_VERSION})`
    );
  }
  if (!Array.isArray(metaObj.series)) {
    throw new Error("bundle: meta.series is missing or not an array");
  }

  // A null-prototype map, so no series name (such as "__proto__") can reach Object.prototype.
  const views = Object.create(null);
  for (const s of metaObj.series) {
    const Ctor = Object.hasOwn(TYPE_VIEWS, s.type) ? TYPE_VIEWS[s.type] : undefined;
    if (!Ctor) throw new Error(`bundle: series ${s.name} has unknown type ${s.type}`);
    if (typeof s.name !== "string" || s.name in views) {
      throw new Error(`bundle: series name ${s.name} is missing or repeated`);
    }
    if (!Number.isSafeInteger(s.offset) || !Number.isSafeInteger(s.count) || s.offset < 0 || s.count < 0) {
      throw new Error(`bundle: series ${s.name} has an invalid offset or count`);
    }
    const bytesPer = Ctor.BYTES_PER_ELEMENT;
    const end = s.offset + s.count * bytesPer;
    if (s.offset % bytesPer !== 0) {
      throw new Error(`bundle: series ${s.name} offset ${s.offset} is not aligned for ${s.type}`);
    }
    if (end > arrayBuffer.byteLength) {
      throw new Error(
        `bundle: series ${s.name} runs past end of data.bin (${end} > ${arrayBuffer.byteLength})`
      );
    }
    views[s.name] = new Ctor(arrayBuffer, s.offset, s.count);
  }

  checkShape(metaObj, views);
  return { meta: metaObj, views, series: metaObj.series };
}

// The largest heatmap the dashboard will draw (rows x cols cells; the 1-hour demo has about 90,000).
export const MAX_HEATMAP_CELLS = 64 * 1024 * 1024;
export const MAX_LEVELS = 10000;

// Every series the dashboard reads must exist with consistent lengths, and the heatmap's
// rows x cols must equal its series length, so a crafted meta.json cannot make the page allocate a
// canvas far larger than data.bin or index past a series (D-903).
function checkShape(meta, views) {
  const need = (name, length) => {
    const v = views[name];
    if (!v) throw new Error(`bundle: series ${name} is missing`);
    if (length !== undefined && v.length !== length) {
      throw new Error(`bundle: series ${name} has ${v.length} values, expected ${length}`);
    }
    return v;
  };
  const levels = meta.sampling && meta.sampling.levels;
  if (!Number.isSafeInteger(levels) || levels < 1 || levels > MAX_LEVELS) {
    throw new Error(`bundle: sampling.levels must be an integer from 1 to ${MAX_LEVELS}`);
  }
  const n = need("ladder_time_ns").length;
  for (const name of ["best_bid", "best_ask", "spread", "mid", "mm_bid", "mm_ask", "mm_inventory", "mm_cash",
                      "mm_pnl", "fundamental"]) {
    need(name, n);
  }
  for (let d = 0; d < levels; d++) {
    for (const k of ["bid_px_", "bid_sz_", "ask_px_", "ask_sz_"]) need(k + d, n);
  }
  const trades = need("trade_time_ns").length;
  for (const name of ["trade_price", "trade_qty", "trade_aggressor"]) need(name, trades);
  const hm = meta.heatmap;
  if (!hm || !Number.isSafeInteger(hm.rows) || !Number.isSafeInteger(hm.cols) || hm.rows < 1 || hm.cols < 0 ||
      hm.rows * hm.cols > MAX_HEATMAP_CELLS) {
    throw new Error(`bundle: heatmap rows and cols must be integers with rows x cols at most ${MAX_HEATMAP_CELLS}`);
  }
  need("heatmap", hm.rows * hm.cols);
}
