// log.test.js — decodeLogCode is the inverse of the exporter's u8 log encoding.
//
// Encoder (viz/viz_core.cpp encode_log_u8):
//   v <= 0            -> 0
//   0 < v             -> clamp(round(255 * ln(1+v) / ln(1+vMax)), 1, 255)
// Decoder (web/js/bundle.js decodeLogCode):
//   code <= 0         -> 0
//   code 1..255       -> expm1(code/255 * log1p(vMax))
import test from "node:test";
import assert from "node:assert/strict";

import { decodeLogCode } from "../js/bundle.js";

// Mirror of the C++ encoder, for round-trip checks.
function encodeLogU8(v, vMax) {
  if (!(v > 0)) return 0;
  if (!(vMax > 0)) return 0;
  let r = Math.round((255 * Math.log1p(v)) / Math.log1p(vMax));
  if (r < 1) r = 1;
  if (r > 255) r = 255;
  return r;
}

const vMax = 251.6593688847324; // v_max from the committed fixture

test("code 0 decodes to exactly 0", () => {
  assert.equal(decodeLogCode(0, vMax), 0);
});

test("code 255 decodes to approximately v_max", () => {
  const v = decodeLogCode(255, vMax);
  const relErr = Math.abs(v - vMax) / vMax;
  assert.ok(relErr < 1e-12, `decode(255) = ${v}, expected ~${vMax} (rel err ${relErr})`);
});

test("decode is monotonic increasing in the code", () => {
  let prev = -1;
  for (let c = 0; c <= 255; c++) {
    const v = decodeLogCode(c, vMax);
    assert.ok(v > prev, `decode(${c}) = ${v} must exceed decode(${c - 1}) = ${prev}`);
    prev = v;
  }
});

test("encode then decode recovers the code for every code", () => {
  for (let c = 1; c <= 255; c++) {
    const v = decodeLogCode(c, vMax);
    assert.equal(encodeLogU8(v, vMax), c, `round-trip for code ${c} (v=${v})`);
  }
});

test("decode then encode holds at a few sample sizes", () => {
  for (const v of [1, 2, 5, 17, 64, 200, vMax]) {
    const code = encodeLogU8(v, vMax);
    const back = decodeLogCode(code, vMax);
    // Decoding a code is lossy, so compare the re-encoded code instead of the raw size.
    assert.equal(encodeLogU8(back, vMax), code, `stable code for v=${v}`);
  }
});

test("a non-positive v_max decodes to 0", () => {
  assert.equal(decodeLogCode(100, 0), 0);
  assert.equal(decodeLogCode(100, -1), 0);
});
