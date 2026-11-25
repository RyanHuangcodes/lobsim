// decoder.test.js — load the committed fixture bundle and check the typed-array decoder.
//
// Run with: node --test web/test   (no packages; node:test + node:assert are built in).
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

import {
  SCHEMA_VERSION,
  MISSING_I32,
  isMissingI32,
  hasMM,
  loadBundle,
} from "../js/bundle.js";

const here = dirname(fileURLToPath(import.meta.url));
const fixtureDir = join(here, "fixtures");

function loadFixture() {
  const meta = JSON.parse(readFileSync(join(fixtureDir, "meta.json"), "utf8"));
  const buf = readFileSync(join(fixtureDir, "data.bin"));
  // Copy into a standalone ArrayBuffer so the typed-array offsets line up from zero.
  const ab = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
  return { meta, ab };
}

test("fixture schema_version matches the decoder's supported version", () => {
  const { meta } = loadFixture();
  assert.equal(meta.schema_version, SCHEMA_VERSION);
});

test("loadBundle builds one typed-array view per series with the right length", () => {
  const { meta, ab } = loadFixture();
  const { views, series } = loadBundle(meta, ab);
  assert.equal(series.length, meta.series.length);
  for (const s of series) {
    assert.ok(views[s.name], `view ${s.name} exists`);
    assert.equal(views[s.name].length, s.count, `length of ${s.name}`);
  }
});

test("series offsets are aligned to their element size", () => {
  const { meta } = loadFixture();
  const bytesPer = { f64: 8, i32: 4, u8: 1 };
  for (const s of meta.series) {
    const per = bytesPer[s.type];
    assert.ok(per, `known type for ${s.name}`);
    assert.equal(s.offset % per, 0, `${s.name} offset ${s.offset} aligned for ${s.type}`);
    // f64 and i32 arrays are padded to 8-byte boundaries by the writer.
    if (s.type !== "u8") assert.equal(s.offset % 8, 0, `${s.name} offset is 8-aligned`);
  }
});

test("declared data_bytes equals data.bin size and the last series ends within it", () => {
  const { meta, ab } = loadFixture();
  assert.equal(meta.data_bytes, ab.byteLength);
  const bytesPer = { f64: 8, i32: 4, u8: 1 };
  for (const s of meta.series) {
    const end = s.offset + s.count * bytesPer[s.type];
    assert.ok(end <= ab.byteLength, `${s.name} ends within data.bin`);
  }
});

test("ladder series share the ladder_count length", () => {
  const { meta, ab } = loadFixture();
  const { views } = loadBundle(meta, ab);
  const L = meta.ladder_count;
  for (const name of ["ladder_time_ns", "best_bid", "best_ask", "spread", "mid", "mm_inventory", "fundamental"]) {
    assert.equal(views[name].length, L, `${name} has ladder_count length`);
  }
  assert.equal(views.ladder_time_ns.constructor, Float64Array);
  assert.equal(views.best_bid.constructor, Int32Array);
});

test("trade series share the trade_count length and aggressor is 0 or 1", () => {
  const { meta, ab } = loadFixture();
  const { views } = loadBundle(meta, ab);
  const T = meta.trade_count;
  for (const name of ["trade_time_ns", "trade_price", "trade_qty", "trade_aggressor"]) {
    assert.equal(views[name].length, T, `${name} has trade_count length`);
  }
  for (let i = 0; i < views.trade_aggressor.length; i++) {
    const a = views.trade_aggressor[i];
    assert.ok(a === 0 || a === 1, `aggressor[${i}] is 0 (buy) or 1 (sell), got ${a}`);
  }
});

test("heatmap length equals rows * cols", () => {
  const { meta, ab } = loadFixture();
  const { views } = loadBundle(meta, ab);
  assert.equal(views.heatmap.constructor, Uint8Array);
  assert.equal(views.heatmap.length, meta.heatmap.rows * meta.heatmap.cols);
});

test("isMissingI32 recognises the INT32_MIN sentinel", () => {
  assert.equal(MISSING_I32, meta_missing());
  assert.ok(isMissingI32(MISSING_I32));
  assert.ok(!isMissingI32(0));
  assert.ok(!isMissingI32(-1));
  assert.ok(!isMissingI32(2147483647));
  function meta_missing() {
    const { meta } = loadFixture();
    return meta.missing_i32;
  }
});

test("the fixture has an MM, so mm series are not entirely missing", () => {
  const { meta, ab } = loadFixture();
  const { views } = loadBundle(meta, ab);
  assert.ok(meta.config && meta.config.mm, "fixture config has an mm block");
  assert.ok(hasMM(meta, views), "hasMM is true for an MM run");
  let anyReal = false;
  for (let i = 0; i < views.mm_inventory.length; i++) {
    if (!isMissingI32(views.mm_inventory[i])) {
      anyReal = true;
      break;
    }
  }
  assert.ok(anyReal, "at least one mm_inventory sample is a real value");
});

test("loadBundle rejects a wrong schema_version", () => {
  const { meta, ab } = loadFixture();
  const bad = { ...meta, schema_version: meta.schema_version + 1 };
  assert.throws(() => loadBundle(bad, ab), /schema_version/);
});

// A crafted bundle must be refused when loaded, not when the page draws it (D-903).
function craft(edit) {
  const { meta, ab } = loadFixture();
  const m = structuredClone(meta);
  edit(m);
  return () => loadBundle(m, ab);
}

test("loadBundle refuses a heatmap larger than its series", () => {
  assert.throws(craft((m) => { m.heatmap.rows = 16000; m.heatmap.cols = 16000; }), /heatmap/);
  assert.throws(craft((m) => { m.heatmap.rows = 1.5; }), /heatmap/);
  assert.throws(craft((m) => { m.heatmap.cols += 1; }), /heatmap/);
});

test("loadBundle refuses missing or short series and a bad level count", () => {
  assert.throws(craft((m) => { m.series = m.series.filter((s) => s.name !== "mid"); }), /mid is missing/);
  assert.throws(craft((m) => { m.series.find((s) => s.name === "mm_pnl").count -= 1; }), /mm_pnl has/);
  assert.throws(craft((m) => { m.sampling.levels = 1e9; }), /levels/);
  assert.throws(craft((m) => { m.sampling.levels = m.sampling.levels + 1; }), /is missing/);
});

test("loadBundle refuses repeated, prototype and malformed series entries", () => {
  assert.throws(craft((m) => { m.series.push({ ...m.series[0] }); }), /repeated/);
  assert.throws(craft((m) => { m.series.push({ name: "x", type: "constructor", offset: 0, count: 1 }); }), /unknown type/);
  assert.throws(craft((m) => { m.series[0].count = -1; }), /invalid offset or count/);
  const { meta, ab } = loadFixture();
  const m = structuredClone(meta);
  m.series.push({ name: "__proto__", type: "u8", offset: 0, count: 1 });
  const { views } = loadBundle(m, ab);
  assert.equal(Object.getPrototypeOf(views), null);
  assert.equal(Object.prototype.polluted, undefined);
});
