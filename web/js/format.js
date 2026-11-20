// format.js — small pure formatters for time and price. Dependency-free, node-importable.

/**
 * Format a nanosecond timestamp as a clock string relative to zero.
 *   < 1 hour : mm:ss.mmm   (e.g. 02:05.250)
 *   >= 1 hour: h:mm:ss.mmm (e.g. 1:02:05.250)
 * @param {number} ns nanoseconds
 */
export function formatTime(ns) {
  if (!Number.isFinite(ns)) return "--:--";
  const neg = ns < 0;
  let ms = Math.floor(Math.abs(ns) / 1e6);
  const millis = ms % 1000;
  let totalSec = Math.floor(ms / 1000);
  const sec = totalSec % 60;
  let totalMin = Math.floor(totalSec / 60);
  const min = totalMin % 60;
  const hours = Math.floor(totalMin / 60);

  const pad = (n, w) => String(n).padStart(w, "0");
  const msPart = pad(millis, 3);
  let out;
  if (hours > 0) {
    out = `${hours}:${pad(min, 2)}:${pad(sec, 2)}.${msPart}`;
  } else {
    out = `${pad(min, 2)}:${pad(sec, 2)}.${msPart}`;
  }
  return neg ? "-" + out : out;
}

/**
 * Format a price in integer ticks to display units.
 * tickDisplay is "price units per tick" (e.g. 0.01). When it is a clean power of
 * ten, the result is fixed to the matching number of decimals.
 * @param {number} ticks integer price in ticks
 * @param {number} tickDisplay display scale (meta.tick_display)
 */
export function formatPrice(ticks, tickDisplay) {
  if (!Number.isFinite(ticks)) return "--";
  const scale = Number.isFinite(tickDisplay) && tickDisplay > 0 ? tickDisplay : 1;
  const value = ticks * scale;
  let decimals = 0;
  if (scale < 1) {
    // derive decimals from the scale, e.g. 0.01 -> 2
    decimals = Math.max(0, Math.round(-Math.log10(scale)));
    if (decimals > 8) decimals = 8;
  }
  return value.toFixed(decimals);
}

/** Compact integer with thousands separators (for sizes / counts). */
export function formatInt(n) {
  if (!Number.isFinite(n)) return "--";
  return Math.round(n).toLocaleString("en-US");
}

/** Signed number with fixed decimals, for PnL-style readouts. */
export function formatSigned(n, decimals = 2) {
  if (!Number.isFinite(n)) return "--";
  const s = n.toFixed(decimals);
  return n > 0 ? "+" + s : s;
}
