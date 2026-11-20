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

  const views = {};
  for (const s of metaObj.series) {
    const Ctor = TYPE_VIEWS[s.type];
    if (!Ctor) throw new Error(`bundle: series ${s.name} has unknown type ${s.type}`);
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

  return { meta: metaObj, views, series: metaObj.series };
}
