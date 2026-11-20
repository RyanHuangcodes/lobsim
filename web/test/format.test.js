// format.test.js — formatTime / formatPrice / formatInt / formatSigned string outputs.
import test from "node:test";
import assert from "node:assert/strict";

import { formatTime, formatPrice, formatInt, formatSigned } from "../js/format.js";

test("formatTime shows mm:ss.mmm under one hour", () => {
  assert.equal(formatTime(0), "00:00.000");
  assert.equal(formatTime(1_500_000_000), "00:01.500"); // 1.5 s
  assert.equal(formatTime(61_500_000_000), "01:01.500"); // 1 min 1.5 s
  assert.equal(formatTime(125_250_000_000), "02:05.250"); // 2 min 5.25 s
});

test("formatTime switches to h:mm:ss.mmm at one hour", () => {
  assert.equal(formatTime(3_600_000_000_000), "1:00:00.000");
  assert.equal(formatTime(3_725_250_000_000), "1:02:05.250");
});

test("formatTime handles negatives and non-finite input", () => {
  assert.equal(formatTime(-1_500_000_000), "-00:01.500");
  assert.equal(formatTime(NaN), "--:--");
  assert.equal(formatTime(Infinity), "--:--");
});

test("formatPrice scales ticks by tick_display with derived decimals", () => {
  assert.equal(formatPrice(10000, 0.01), "100.00");
  assert.equal(formatPrice(10011, 0.01), "100.11");
  assert.equal(formatPrice(9982, 0.01), "99.82");
  assert.equal(formatPrice(5, 0.01), "0.05");
});

test("formatPrice with unit tick scale prints an integer", () => {
  assert.equal(formatPrice(42, 1), "42");
  assert.equal(formatPrice(42), "42"); // default scale 1
});

test("formatPrice returns -- for non-finite ticks", () => {
  assert.equal(formatPrice(NaN, 0.01), "--");
});

test("formatInt groups thousands", () => {
  assert.equal(formatInt(0), "0");
  assert.equal(formatInt(1234567), "1,234,567");
  assert.equal(formatInt(NaN), "--");
});

test("formatSigned prefixes a plus for positive values", () => {
  assert.equal(formatSigned(12.5), "+12.50");
  assert.equal(formatSigned(-12.5), "-12.50");
  assert.equal(formatSigned(0), "0.00");
  assert.equal(formatSigned(3.14159, 3), "+3.142");
  assert.equal(formatSigned(NaN), "--");
});
