// search.js — binary search over sorted time arrays. Never a linear scan per frame.
//
// Pure ES module, importable by node for unit tests.

/**
 * Index of the sample at or just before tNs in a sorted (ascending) time array.
 *
 * Returns:
 *   -1 when `times` is empty or tNs is before the first sample.
 *   The index i of the greatest times[i] <= tNs otherwise.
 *
 * @param {Float64Array|number[]} times sorted ascending
 * @param {number} tNs target time in nanoseconds
 */
export function binarySearchTime(times, tNs) {
  const n = times.length;
  if (n === 0) return -1;
  if (tNs < times[0]) return -1;
  if (tNs >= times[n - 1]) return n - 1;

  let lo = 0;
  let hi = n - 1; // times[hi] > tNs is not guaranteed yet, but loop narrows it
  // Invariant: times[lo] <= tNs < times[hi] once we enter the loop body region.
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1;
    if (times[mid] <= tNs) lo = mid;
    else hi = mid;
  }
  return lo;
}

/**
 * Index of the first sample at or after tNs (the "ceiling"), or times.length when
 * every sample is before tNs. Useful for range selection together with binarySearchTime.
 */
export function lowerBoundTime(times, tNs) {
  const n = times.length;
  let lo = 0;
  let hi = n;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (times[mid] < tNs) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}
