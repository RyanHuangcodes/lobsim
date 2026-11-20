// heatmap.js — one-hue blue log heatmap: offscreen-prerendered ImageData, then blitted.
//
// Depends only on bundle.js helpers (pure). The DOM canvas is passed in by app.js.

import { decodeLogCode, heatmapIndex, rowToPrice, colToTimeNs } from "./bundle.js";

// The validated blue ramp steps (size 0 is the surface colour, injected at runtime).
// Light mode runs surface -> dark blue; dark mode runs surface -> light blue (steps reversed).
const RAMP_STEPS = ["#cde2fb", "#9ec5f4", "#6da7ec", "#3987e5", "#256abf", "#184f95", "#0d366b"];

function hexToRgb(hex) {
  const h = hex.replace("#", "");
  return [
    parseInt(h.slice(0, 2), 16),
    parseInt(h.slice(2, 4), 16),
    parseInt(h.slice(4, 6), 16),
  ];
}

function lerp(a, b, t) {
  return a + (b - a) * t;
}

/**
 * Build the control-point colour list for a theme: surface first, then the blue steps.
 * Light: surface -> #cde2fb ... -> #0d366b
 * Dark:  surface -> #0d366b ... -> #cde2fb  (steps reversed so small sizes recede)
 */
function rampControlPoints(surfaceHex, dark) {
  const steps = dark ? RAMP_STEPS.slice().reverse() : RAMP_STEPS.slice();
  return [surfaceHex, ...steps].map(hexToRgb);
}

/**
 * Precompute a 256-entry RGBA lookup table (Uint8ClampedArray, 256*4).
 * Code 0 -> surface; codes 1..255 interpolate across the control points.
 */
export function buildRampLUT(surfaceHex, dark) {
  const cps = rampControlPoints(surfaceHex, dark);
  const segs = cps.length - 1;
  const lut = new Uint8ClampedArray(256 * 4);
  for (let code = 0; code < 256; code++) {
    const u = code / 255; // 0..1
    const pos = u * segs;
    let i = Math.floor(pos);
    if (i >= segs) i = segs - 1;
    const t = pos - i;
    const c0 = cps[i];
    const c1 = cps[i + 1];
    const o = code * 4;
    lut[o] = lerp(c0[0], c1[0], t);
    lut[o + 1] = lerp(c0[1], c1[1], t);
    lut[o + 2] = lerp(c0[2], c1[2], t);
    lut[o + 3] = 255;
  }
  return lut;
}

/**
 * Pre-render the heatmap codes into an offscreen canvas as ImageData, once per zoom level.
 * One RGBA lookup per code; the result is sized rows x cols in natural orientation
 * (x = column/time, y = row/price, row 0 at the top) and blitted scaled by the caller.
 *
 * @param {Uint8Array} codes heatmap series (length rows*cols)
 * @param {number} rows
 * @param {number} cols
 * @param {Uint8ClampedArray} lut 256*4 RGBA table
 * @param {number} col0 first visible column (inclusive)
 * @param {number} col1 last visible column (inclusive)
 * @returns {HTMLCanvasElement|OffscreenCanvas} offscreen canvas of size (col1-col0+1) x rows
 */
export function prerenderHeatmap(codes, rows, cols, lut, col0, col1) {
  const c0 = Math.max(0, col0);
  const c1 = Math.min(cols - 1, col1);
  const w = Math.max(1, c1 - c0 + 1);
  const off = makeCanvas(w, rows);
  const ctx = off.getContext("2d");
  const img = ctx.createImageData(w, rows);
  const out = img.data;
  for (let col = c0; col <= c1; col++) {
    const x = col - c0;
    for (let row = 0; row < rows; row++) {
      const code = codes[heatmapIndex(row, col, rows)];
      const src = code * 4;
      const dst = (row * w + x) * 4;
      out[dst] = lut[src];
      out[dst + 1] = lut[src + 1];
      out[dst + 2] = lut[src + 2];
      out[dst + 3] = lut[src + 3];
    }
  }
  ctx.putImageData(img, 0, 0);
  return off;
}

function makeCanvas(w, h) {
  if (typeof OffscreenCanvas !== "undefined") return new OffscreenCanvas(w, h);
  const c = document.createElement("canvas");
  c.width = w;
  c.height = h;
  return c;
}

/**
 * HeatmapView owns a visible canvas and an offscreen pre-render. It re-renders the
 * offscreen buffer only when the zoom (visible column range) or theme changes, then
 * blits + draws overlays (best bid/ask lines, trade dots) each frame cheaply.
 */
export class HeatmapView {
  constructor(canvas, meta) {
    this.canvas = canvas;
    this.ctx = canvas.getContext("2d");
    this.meta = meta;
    this.hm = meta.heatmap;
    this.codes = null; // set in setData
    this.lut = null;
    this.off = null;
    this.col0 = 0;
    this.col1 = this.hm.cols - 1;
    this.dark = false;
    this.surface = "#fcfcfb";
  }

  setData(codes) {
    this.codes = codes;
  }

  setTheme(surfaceHex, dark) {
    this.surface = surfaceHex;
    this.dark = dark;
    this.lut = buildRampLUT(surfaceHex, dark);
    this.off = null; // force re-render
  }

  /** Set the visible column range (zoom). Triggers a re-render on next draw. */
  setColumnRange(col0, col1) {
    const c0 = Math.max(0, Math.floor(col0));
    const c1 = Math.min(this.hm.cols - 1, Math.ceil(col1));
    if (c0 !== this.col0 || c1 !== this.col1) {
      this.col0 = c0;
      this.col1 = c1;
      this.off = null;
    }
  }

  /** Map a time in ns to a fractional column index within the visible window. */
  timeToX(tNs, width) {
    const { t0_ns, dt_ns } = this.hm;
    const colf = (tNs - t0_ns) / dt_ns;
    const span = this.col1 - this.col0 + 1;
    return ((colf - this.col0) / span) * width;
  }

  /** Map a price in ticks to a y pixel. */
  priceToY(price, height) {
    const { price_hi, rows } = this.hm;
    const row = price_hi - price; // 0 at top
    return ((row + 0.5) / rows) * height;
  }

  ensureRender() {
    if (!this.off && this.codes && this.lut) {
      this.off = prerenderHeatmap(this.codes, this.hm.rows, this.hm.cols, this.lut, this.col0, this.col1);
    }
  }

  /**
   * Blit the pre-rendered buffer scaled to the canvas, then draw overlays.
   * @param {object} overlay {bbTimes, bb, ba, trades:{t,p,q,aggr}, colors:{bid,ask,buy,sell}, qtyCap}
   */
  draw(overlay) {
    const { canvas, ctx } = this;
    const w = canvas.width;
    const h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    this.ensureRender();
    if (this.off) {
      ctx.imageSmoothingEnabled = false;
      ctx.drawImage(this.off, 0, 0, this.off.width, this.off.height, 0, 0, w, h);
    }
    if (overlay) this.drawOverlay(overlay, w, h);
  }

  drawOverlay(ov, w, h) {
    const ctx = this.ctx;
    const colors = ov.colors || {};
    // Best bid / best ask thin lines over the visible window.
    const t0 = colToTimeNs(this.col0, this.hm.t0_ns, this.hm.dt_ns);
    const t1 = colToTimeNs(this.col1 + 1, this.hm.t0_ns, this.hm.dt_ns);
    if (ov.ladderTimes && ov.bb) this.drawStepLine(ov.ladderTimes, ov.bb, colors.bid || "#2a78d6", t0, t1, w, h);
    if (ov.ladderTimes && ov.ba) this.drawStepLine(ov.ladderTimes, ov.ba, colors.ask || "#eb6834", t0, t1, w, h);

    // Trade dots, area scaled by qty with a cap.
    if (ov.trades) {
      const { t, p, q, aggr } = ov.trades;
      const cap = ov.qtyCap || 50;
      for (let i = 0; i < t.length; i++) {
        const tt = t[i];
        if (tt < t0 || tt > t1) continue;
        const x = this.timeToX(tt, w);
        const y = this.priceToY(p[i], h);
        const r = this.dotRadius(q[i], cap);
        ctx.beginPath();
        ctx.arc(x, y, r, 0, Math.PI * 2);
        ctx.fillStyle = aggr[i] === 0 ? colors.buy || "#2a78d6" : colors.sell || "#eb6834";
        ctx.globalAlpha = 0.85;
        ctx.fill();
        ctx.globalAlpha = 1;
      }
    }
  }

  dotRadius(qty, cap) {
    const clamped = Math.min(qty, cap);
    // area ∝ qty; min diameter 8px => min radius 4px.
    const minR = 4;
    const maxR = 11;
    const t = cap > 0 ? clamped / cap : 0;
    return minR + (maxR - minR) * Math.sqrt(t);
  }

  drawStepLine(times, vals, color, t0, t1, w, h) {
    const ctx = this.ctx;
    ctx.beginPath();
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.25;
    let started = false;
    for (let i = 0; i < times.length; i++) {
      const tt = times[i];
      if (tt < t0 || tt > t1) continue;
      const v = vals[i];
      if (v === -2147483648) {
        started = false;
        continue;
      }
      const x = this.timeToX(tt, w);
      const y = this.priceToY(v, h);
      if (!started) {
        ctx.moveTo(x, y);
        started = true;
      } else {
        ctx.lineTo(x, y);
      }
    }
    ctx.stroke();
  }
}

/**
 * Draw the colour legend onto a small canvas, labelled in real sizes via decodeLogCode.
 * Renders a vertical gradient strip (code 255 at top, 0 at bottom) and a few tick labels.
 */
export function drawLegend(canvas, lut, vMax, textColor) {
  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  const stripW = Math.min(18, w * 0.4);
  for (let y = 0; y < h; y++) {
    const code = Math.round((1 - y / (h - 1)) * 255);
    const o = code * 4;
    ctx.fillStyle = `rgb(${lut[o]},${lut[o + 1]},${lut[o + 2]})`;
    ctx.fillRect(0, y, stripW, 1);
  }
  ctx.fillStyle = textColor || "#898781";
  ctx.font = "10px system-ui, sans-serif";
  ctx.textBaseline = "middle";
  const ticks = [255, 191, 128, 64, 1];
  for (const code of ticks) {
    const y = (1 - code / 255) * (h - 1);
    const size = decodeLogCode(code, vMax);
    ctx.fillText(size >= 10 ? size.toFixed(0) : size.toFixed(1), stripW + 4, y);
  }
}

export { rowToPrice, colToTimeNs };
