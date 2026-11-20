// search.test.js — edge and property tests for binarySearchTime / lowerBoundTime.
import test from "node:test";
import assert from "node:assert/strict";

import { binarySearchTime, lowerBoundTime } from "../js/search.js";

const times = new Float64Array([10, 20, 30, 40, 50]);

test("binarySearchTime returns -1 on an empty array", () => {
  assert.equal(binarySearchTime(new Float64Array([]), 5), -1);
});

test("binarySearchTime returns -1 before the first sample", () => {
  assert.equal(binarySearchTime(times, 9), -1);
  assert.equal(binarySearchTime(times, -1000), -1);
});

test("binarySearchTime returns the last index at or after the last sample", () => {
  assert.equal(binarySearchTime(times, 50), 4);
  assert.equal(binarySearchTime(times, 999), 4);
});

test("binarySearchTime hits exact samples", () => {
  assert.equal(binarySearchTime(times, 10), 0);
  assert.equal(binarySearchTime(times, 20), 1);
  assert.equal(binarySearchTime(times, 30), 2);
  assert.equal(binarySearchTime(times, 40), 3);
});

test("binarySearchTime returns the sample just before a value between samples", () => {
  assert.equal(binarySearchTime(times, 25), 1); // between 20 and 30 -> 20
  assert.equal(binarySearchTime(times, 39), 2); // between 30 and 40 -> 30
  assert.equal(binarySearchTime(times, 11), 0);
});

test("binarySearchTime matches a linear at-or-before scan for a random array", () => {
  const arr = [];
  let t = 0;
  for (let i = 0; i < 200; i++) {
    t += 1 + Math.floor(Math.random() * 7);
    arr.push(t);
  }
  const ta = Float64Array.from(arr);
  const linear = (q) => {
    let idx = -1;
    for (let i = 0; i < ta.length; i++) {
      if (ta[i] <= q) idx = i;
      else break;
    }
    return idx;
  };
  for (let q = -5; q <= t + 5; q++) {
    assert.equal(binarySearchTime(ta, q), linear(q), `query ${q}`);
  }
});

test("lowerBoundTime returns the first index at or after the target", () => {
  assert.equal(lowerBoundTime(times, 5), 0);
  assert.equal(lowerBoundTime(times, 10), 0);
  assert.equal(lowerBoundTime(times, 11), 1);
  assert.equal(lowerBoundTime(times, 30), 2);
  assert.equal(lowerBoundTime(times, 50), 4);
  assert.equal(lowerBoundTime(times, 51), 5); // past the end
});
