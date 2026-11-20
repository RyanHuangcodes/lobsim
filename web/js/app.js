// app.js — dashboard controller. Wires loading, the shared cursor, zoom, playback,
// keyboard, table views and the light/dark toggle across every panel.

import { loadBundle, hasMM, isMissingI32, colToTimeNs, SCHEMA_VERSION } from "./bundle.js";
import { binarySearchTime, lowerBoundTime } from "./search.js";
import { formatTime, formatPrice, formatInt, formatSigned } from "./format.js";
import { HeatmapView, buildRampLUT, drawLegend } from "./heatmap.js";

const uPlot = window.uPlot;

// --- theme palette (must mirror css/style.css; charts read JS, not CSS, for series colours) ---
const PALETTE = {
  light: { surface: "#fcfcfb", ink: "#0b0b0b", ink2: "#52514e", muted: "#898781", grid: "#e1e0d9", buy: "#2a78d6", sell: "#eb6834", mm: "#1baf7a" },
  dark: { surface: "#1a1a19", ink: "#f0efec", ink2: "#c3c2b7", muted: "#898781", grid: "#2c2c2a", buy: "#3987e5", sell: "#d95926", mm: "#199e70" },
};

const state = {
  meta: null,
  views: null,
  hasMM: false,
  times: null, // ladder_time_ns Float64Array
  cursorTime: 0,
  zoom: null, // [t0, t1] ns, null = full range
  dark: false,
  playing: false,
  speed: 60,
  rafId: null,
  lastFrameMs: 0,
  heatmap: null, // HeatmapView
  charts: {}, // price, inventory, pnl -> uPlot instances
  qtyCap: 50,
};

const el = (id) => document.getElementById(id);

// -------------------------------------------------------------------------- loading
async function loadFromRunParam(name) {
  setStatus(`Loading run “${name}”…`);
  const base = `runs/${encodeURIComponent(name)}`;
  const [metaRes, binRes] = await Promise.all([fetch(`${base}/meta.json`), fetch(`${base}/data.bin`)]);
  if (!metaRes.ok) throw new Error(`cannot fetch ${base}/meta.json (${metaRes.status})`);
  if (!binRes.ok) throw new Error(`cannot fetch ${base}/data.bin (${binRes.status})`);
  const meta = await metaRes.json();
  const buf = await binRes.arrayBuffer();
  initBundle(meta, buf);
}

async function loadFromFiles(fileList) {
  const files = Array.from(fileList);
  const metaFile = files.find((f) => f.name.endsWith(".json"));
  const binFile = files.find((f) => f.name.endsWith(".bin"));
  if (!metaFile || !binFile) throw new Error("select both meta.json and data.bin");
  setStatus("Reading files…");
  const meta = JSON.parse(await metaFile.text());
  const buf = await binFile.arrayBuffer();
  initBundle(meta, buf);
}

function initBundle(meta, buf) {
  const t0 = performance.now();
  let bundle;
  try {
    bundle = loadBundle(meta, buf);
  } catch (e) {
    setError(e.message);
    return;
  }
  state.meta = bundle.meta;
  state.views = bundle.views;
  state.hasMM = hasMM(bundle.meta, bundle.views);
  state.times = bundle.views.ladder_time_ns;
  state.cursorTime = state.times[0];
  state.zoom = null;
  state.playing = false;
  state.qtyCap = computeQtyCap(bundle.views.trade_qty);

  fillHeader();
  toggleMMPanels(state.hasMM);
  buildHeatmap();
  buildCharts();
  setupSlider();
  render();
  clearStatus();
  const firstRenderMs = performance.now() - t0;
  // Perf-budget instrumentation: load -> first render.
  console.log(`[perf] decode+first-render: ${firstRenderMs.toFixed(1)} ms (ladder=${state.times.length}, trades=${bundle.views.trade_time_ns.length}, heatmap=${meta.heatmap.rows}x${meta.heatmap.cols})`);
}

function computeQtyCap(qty) {
  // cap at the 95th-percentile-ish: use max but clamp the scaling effect.
  let max = 1;
  for (let i = 0; i < qty.length; i++) if (qty[i] > max) max = qty[i];
  return Math.max(10, Math.min(max, 100));
}

// -------------------------------------------------------------------------- header
function fillHeader() {
  const { meta, views } = state;
  el("run-title").textContent = `lobsim · ${meta.run_name}`;
  el("stat-seed").textContent = meta.seed;
  const durS = meta.config && meta.config.duration_s;
  el("stat-duration").textContent = Number.isFinite(durS) ? `${durS}s` : formatTime(meta.time_range_ns[1] - meta.time_range_ns[0]);
  el("stat-fills").textContent = formatInt(meta.trade_count);

  if (state.hasMM) {
    const pnl = lastFinite(views.mm_pnl);
    const inv = lastNonMissing(views.mm_inventory);
    el("stat-mm-pnl").textContent = Number.isFinite(pnl) ? formatSigned(pnl, 1) : "--";
    el("stat-inv").textContent = Number.isFinite(inv) ? String(inv) : "--";
    el("stat-mm-pnl-wrap").classList.remove("hidden");
    el("stat-inv-wrap").classList.remove("hidden");
  } else {
    el("stat-mm-pnl-wrap").classList.add("hidden");
    el("stat-inv-wrap").classList.add("hidden");
  }
}

function lastFinite(arr) {
  for (let i = arr.length - 1; i >= 0; i--) if (Number.isFinite(arr[i])) return arr[i];
  return NaN;
}
function lastNonMissing(arr) {
  for (let i = arr.length - 1; i >= 0; i--) if (!isMissingI32(arr[i])) return arr[i];
  return NaN;
}

function toggleMMPanels(on) {
  el("panel-inventory").classList.toggle("hidden", !on);
  el("panel-pnl").classList.toggle("hidden", !on);
}

// -------------------------------------------------------------------------- heatmap
function buildHeatmap() {
  const canvas = el("heatmap-canvas");
  // size the backing store to the displayed size for crisp blitting
  const rect = canvas.getBoundingClientRect();
  canvas.width = Math.max(300, Math.floor(rect.width));
  canvas.height = 360;
  const hv = new HeatmapView(canvas, state.meta);
  hv.setData(state.views.heatmap);
  hv.setTheme(pal().surface, state.dark);
  state.heatmap = hv;
  applyZoomToHeatmap();
}

function applyZoomToHeatmap() {
  const hv = state.heatmap;
  if (!hv) return;
  const hm = state.meta.heatmap;
  if (!state.zoom) {
    hv.setColumnRange(0, hm.cols - 1);
  } else {
    const c0 = (state.zoom[0] - hm.t0_ns) / hm.dt_ns;
    const c1 = (state.zoom[1] - hm.t0_ns) / hm.dt_ns;
    hv.setColumnRange(c0, c1);
  }
}

function drawHeatmap() {
  const hv = state.heatmap;
  if (!hv) return;
  const p = pal();
  hv.draw({
    ladderTimes: state.times,
    bb: state.views.best_bid,
    ba: state.views.best_ask,
    trades: {
      t: state.views.trade_time_ns,
      p: state.views.trade_price,
      q: state.views.trade_qty,
      aggr: state.views.trade_aggressor,
    },
    colors: { bid: p.buy, ask: p.sell, buy: p.buy, sell: p.sell },
    qtyCap: state.qtyCap,
  });
  const lut = buildRampLUT(p.surface, state.dark);
  drawLegend(el("heatmap-legend"), lut, state.meta.heatmap.v_max, p.muted);
}

// -------------------------------------------------------------------------- uPlot charts
function pal() {
  return state.dark ? PALETTE.dark : PALETTE.light;
}

function maskMissingI32(arr) {
  const out = new Array(arr.length);
  for (let i = 0; i < arr.length; i++) out[i] = isMissingI32(arr[i]) ? null : arr[i];
  return out;
}
function maskNaN(arr) {
  const out = new Array(arr.length);
  for (let i = 0; i < arr.length; i++) out[i] = Number.isFinite(arr[i]) ? arr[i] : null;
  return out;
}

let cursorSyncKey;

function makeChart(host, series, data, opts = {}) {
  const p = pal();
  const width = host.getBoundingClientRect().width || 900;
  const o = {
    width: Math.max(300, Math.floor(width)),
    height: opts.height || 180,
    cursor: {
      sync: { key: cursorSyncKey, setSeries: false },
      drag: { x: true, y: false },
    },
    scales: { x: { time: false } },
    axes: [
      { stroke: p.muted, grid: { stroke: p.grid, width: 1, dash: [] }, ticks: { stroke: p.grid }, values: (u, vals) => vals.map((v) => formatTime(v)) },
      { stroke: p.muted, grid: { stroke: p.grid, width: 1, dash: [] }, ticks: { stroke: p.grid } },
    ],
    series,
    hooks: {
      setCursor: [(u) => onChartCursor(u)],
      setScale: [(u, key) => { if (key === "x") onChartZoom(u); }],
    },
  };
  if (opts.zeroBaseline) {
    o.scales.y = { range: (u, min, max) => [Math.min(0, min), Math.max(0, max)] };
  }
  return new uPlot(o, data, host);
}

function buildCharts() {
  // tear down any prior instances
  for (const k of Object.keys(state.charts)) {
    if (state.charts[k]) state.charts[k].destroy();
  }
  state.charts = {};
  cursorSyncKey = uPlot.sync ? uPlot.sync("lobsim").key : "lobsim";

  const p = pal();
  const xs = Array.from(state.times);
  const V = state.views;

  // Price chart: mid, fundamental, MM bid, MM ask. Trades as dots on an overlay series.
  const fundamental = maskNaN(V.fundamental);
  const fundHasData = fundamental.some((v) => v !== null);
  const priceSeries = [
    {},
    { label: "mid", stroke: p.ink, width: 2, points: { show: false } },
  ];
  const priceData = [xs, maskNaN(V.mid)];
  if (fundHasData) {
    priceSeries.push({ label: "fundamental", stroke: p.ink2, width: 2, points: { show: false } });
    priceData.push(fundamental);
  }
  if (state.hasMM) {
    priceSeries.push({ label: "MM bid", stroke: p.mm, width: 2, dash: [], points: { show: false } });
    priceData.push(maskMissingI32(V.mm_bid));
    priceSeries.push({ label: "MM ask", stroke: p.mm, width: 2, points: { show: true, size: 4 } });
    priceData.push(maskMissingI32(V.mm_ask));
  }

  // Trades as dots. uPlot shares one x-array per chart, so each trade is snapped to the
  // nearest ladder sample and drawn as a points-only series (no connecting line). Two
  // series — "trade buy" and "trade sell" — reuse the existing buy/sell palette colours so
  // no 4th series colour is introduced (the spec forbids one); the split lets uPlot colour
  // each dot by aggressor without per-point stroke callbacks. Point size is qty-scaled with
  // a cap and a >=8px floor, matching the heatmap trade dots.
  const trade = buildTradeOverlaySeries(xs);
  if (trade) {
    priceSeries.push(tradeDotSeries("trade buy", p.buy, trade.sizeBuy));
    priceData.push(trade.buy);
    priceSeries.push(tradeDotSeries("trade sell", p.sell, trade.sizeSell));
    priceData.push(trade.sell);
  }
  state.charts.price = makeChart(el("chart-price"), priceSeries, priceData, { height: 220 });

  if (state.hasMM) {
    state.charts.inventory = makeChart(
      el("chart-inventory"),
      [{}, { label: "inventory", stroke: p.mm, width: 2, points: { show: false } }],
      [xs, maskMissingI32(V.mm_inventory)],
      { zeroBaseline: true }
    );
    state.charts.pnl = makeChart(
      el("chart-pnl"),
      [{}, { label: "PnL (ticks)", stroke: p.ink, width: 2, points: { show: false } }],
      [xs, maskNaN(V.mm_pnl)],
      { zeroBaseline: true }
    );
  }
}

// Build price-chart trade overlay data: two y-arrays (buy / sell) aligned to the ladder
// x-array, plus matching per-index point-size arrays. Each trade is snapped to its nearest
// ladder sample; collisions keep the later trade (points-only, so this only affects the
// drawn dot, never a line). Returns null when there are no trades.
function buildTradeOverlaySeries(xs) {
  const V = state.views;
  const t = V.trade_time_ns;
  const n = t.length;
  if (!n) return null;
  const len = xs.length;
  const buy = new Array(len).fill(null);
  const sell = new Array(len).fill(null);
  const sizeBuy = new Array(len).fill(0);
  const sizeSell = new Array(len).fill(0);
  const cap = state.qtyCap;
  for (let i = 0; i < n; i++) {
    const idx = binarySearchTime(state.times, t[i]);
    if (idx < 0) continue;
    const px = V.trade_price[i];
    const sz = dotDiameter(V.trade_qty[i], cap);
    if (V.trade_aggressor[i] === 0) {
      buy[idx] = px;
      sizeBuy[idx] = Math.max(sizeBuy[idx], sz);
    } else {
      sell[idx] = px;
      sizeSell[idx] = Math.max(sizeSell[idx], sz);
    }
  }
  return { buy, sell, sizeBuy, sizeSell };
}

// A points-only uPlot series whose per-point diameter comes from a size array (index-aligned).
function tradeDotSeries(label, color, sizes) {
  return {
    label,
    stroke: color,
    fill: color,
    width: 0,
    points: {
      show: true,
      stroke: color,
      fill: color,
      size: (u, seriesIdx, i) => sizes[i] || 0,
    },
  };
}

// Trade-dot diameter in px: area ∝ qty with a cap and an 8px floor, mirroring
// HeatmapView.dotRadius (min radius 4 -> 8px diameter, max radius 11 -> 22px diameter).
function dotDiameter(qty, cap) {
  const clamped = Math.min(qty, cap);
  const minR = 4;
  const maxR = 11;
  const t = cap > 0 ? clamped / cap : 0;
  return 2 * (minR + (maxR - minR) * Math.sqrt(t));
}

let syncingCursor = false;
function onChartCursor(u) {
  if (syncingCursor) return;
  const idx = u.cursor.idx;
  if (idx == null) return;
  const t = state.times[idx];
  if (Number.isFinite(t)) setCursorTime(t, u);
}

function onChartZoom(u) {
  const xmin = u.scales.x.min;
  const xmax = u.scales.x.max;
  const full = state.times[0];
  const fullEnd = state.times[state.times.length - 1];
  if (xmin <= full && xmax >= fullEnd) {
    state.zoom = null;
  } else {
    state.zoom = [xmin, xmax];
  }
  applyZoomToHeatmap();
  drawHeatmap();
}

// -------------------------------------------------------------------------- shared cursor
function setCursorTime(tNs, origin) {
  state.cursorTime = tNs;
  el("time-readout").textContent = formatTime(tNs - state.times[0]);
  const idx = binarySearchTime(state.times, tNs);
  syncSlider(idx);

  // move uPlot cursors on the other charts
  syncingCursor = true;
  for (const key of Object.keys(state.charts)) {
    const u = state.charts[key];
    if (!u || u === origin) continue;
    const left = u.valToPos(tNs, "x");
    u.setCursor({ left, top: u.cursor.top != null ? u.cursor.top : 0 });
  }
  syncingCursor = false;

  drawHeatmap();
  drawCursorOnHeatmap();
  updateLadder(idx);
  refreshOpenTableViews(idx);
}

function drawCursorOnHeatmap() {
  const hv = state.heatmap;
  if (!hv) return;
  const ctx = hv.ctx;
  const x = hv.timeToX(state.cursorTime, hv.canvas.width);
  if (x < 0 || x > hv.canvas.width) return;
  ctx.save();
  ctx.strokeStyle = pal().muted;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(x, 0);
  ctx.lineTo(x, hv.canvas.height);
  ctx.stroke();
  ctx.restore();
}

// -------------------------------------------------------------------------- ladder
function updateLadder(idx) {
  const tbody = el("ladder-body");
  if (idx < 0) { tbody.innerHTML = ""; return; }
  const V = state.views;
  const td = state.meta.tick_display;
  const levels = state.meta.sampling.levels;
  const mmBid = state.hasMM ? V.mm_bid[idx] : -2147483648;
  const mmAsk = state.hasMM ? V.mm_ask[idx] : -2147483648;

  // gather sizes for proportional bars
  let maxSz = 1;
  for (let d = 0; d < levels; d++) {
    const bs = V[`bid_sz_${d}`][idx];
    const as = V[`ask_sz_${d}`][idx];
    if (!isMissingI32(bs) && bs > maxSz) maxSz = bs;
    if (!isMissingI32(as) && as > maxSz) maxSz = as;
  }

  const rows = [];
  for (let d = 0; d < levels; d++) {
    const ap = V[`ask_px_${d}`][idx];
    const as = V[`ask_sz_${d}`][idx];
    if (isMissingI32(ap)) continue;
    rows.unshift(ladderRow(ap, NaN, as, td, maxSz, mmBid, mmAsk));
  }
  for (let d = 0; d < levels; d++) {
    const bp = V[`bid_px_${d}`][idx];
    const bs = V[`bid_sz_${d}`][idx];
    if (isMissingI32(bp)) continue;
    rows.push(ladderRow(bp, bs, NaN, td, maxSz, mmBid, mmAsk));
  }
  tbody.innerHTML = rows.join("");
}

function ladderRow(price, bidSz, askSz, td, maxSz, mmBid, mmAsk) {
  const barW = (sz) => (isMissingI32(sz) || !Number.isFinite(sz) ? 0 : Math.round((sz / maxSz) * 60));
  let mmTag = "";
  if (price === mmBid) mmTag = `<span class="mm-tag">MM bid</span>`;
  else if (price === mmAsk) mmTag = `<span class="mm-tag">MM ask</span>`;
  const bidCell = Number.isFinite(bidSz) && !isMissingI32(bidSz)
    ? `${formatInt(bidSz)} <span class="bar bar-bid" style="width:${barW(bidSz)}px"></span>` : "";
  const askCell = Number.isFinite(askSz) && !isMissingI32(askSz)
    ? `<span class="bar bar-ask" style="width:${barW(askSz)}px"></span> ${formatInt(askSz)}` : "";
  return `<tr><td>${bidCell}</td><td class="price">${formatPrice(price, td)}${mmTag}</td><td>${askCell}</td></tr>`;
}

// -------------------------------------------------------------------------- slider
function setupSlider() {
  const slider = el("time-slider");
  slider.min = "0";
  slider.max = String(state.times.length - 1);
  slider.value = "0";
}
function syncSlider(idx) {
  if (idx >= 0) el("time-slider").value = String(idx);
}

// -------------------------------------------------------------------------- table views
function refreshOpenTableViews(idx) {
  document.querySelectorAll(".table-view:not(.hidden)").forEach((div) => {
    renderTableView(div.getAttribute("data-panel-table"), idx, div);
  });
}

function renderTableView(panel, idx, div) {
  const V = state.views;
  const td = state.meta.tick_display;
  const lo = Math.max(0, idx - 20);
  const hi = Math.min(state.times.length - 1, idx + 20);
  let html = "";
  if (panel === "price" || panel === "inventory" || panel === "pnl" || panel === "ladder" || panel === "heatmap") {
    html += "<table><thead><tr><th>time</th><th>mid</th><th>best bid</th><th>best ask</th>";
    if (state.hasMM) html += "<th>MM bid</th><th>MM ask</th><th>inv</th><th>PnL</th>";
    html += "</tr></thead><tbody>";
    for (let i = lo; i <= hi; i++) {
      const cur = i === idx ? ' class="at-cursor"' : "";
      html += `<tr${cur}><td>${formatTime(state.times[i] - state.times[0])}</td>`;
      html += `<td>${fmtF(V.mid[i])}</td>`;
      html += `<td>${fmtI(V.best_bid[i], td)}</td>`;
      html += `<td>${fmtI(V.best_ask[i], td)}</td>`;
      if (state.hasMM) {
        html += `<td>${fmtI(V.mm_bid[i], td)}</td><td>${fmtI(V.mm_ask[i], td)}</td>`;
        html += `<td>${fmtMI(V.mm_inventory[i])}</td><td>${fmtF(V.mm_pnl[i])}</td>`;
      }
      html += "</tr>";
    }
    html += "</tbody></table>";

    // nearby trades, with buy/sell as text (colour is not the only cue)
    const t0 = state.times[lo];
    const t1 = state.times[hi];
    const ti0 = lowerBoundTime(V.trade_time_ns, t0);
    const ti1 = lowerBoundTime(V.trade_time_ns, t1);
    if (ti1 > ti0) {
      html += `<table><thead><tr><th>trade time</th><th>price</th><th>qty</th><th>side</th></tr></thead><tbody>`;
      for (let i = ti0; i < ti1; i++) {
        const buy = V.trade_aggressor[i] === 0;
        html += `<tr><td>${formatTime(V.trade_time_ns[i] - state.times[0])}</td><td>${formatPrice(V.trade_price[i], td)}</td><td>${formatInt(V.trade_qty[i])}</td><td class="${buy ? "buy" : "sell"}">${buy ? "BUY" : "SELL"}</td></tr>`;
      }
      html += "</tbody></table>";
    }
  }
  div.innerHTML = html;
}

const fmtF = (v) => (Number.isFinite(v) ? v.toFixed(1) : "·");
const fmtI = (v, td) => (isMissingI32(v) ? "·" : formatPrice(v, td));
const fmtMI = (v) => (isMissingI32(v) ? "·" : String(v));

// -------------------------------------------------------------------------- playback
function togglePlay() {
  state.playing = !state.playing;
  const btn = el("btn-play");
  btn.setAttribute("aria-pressed", String(state.playing));
  btn.textContent = state.playing ? "⏸ Pause" : "▶ Play";
  if (state.playing) {
    state.lastFrameMs = performance.now();
    state.rafId = requestAnimationFrame(playStep);
  } else if (state.rafId) {
    cancelAnimationFrame(state.rafId);
    state.rafId = null;
  }
}

function playStep(nowMs) {
  if (!state.playing) return;
  const dtMs = nowMs - state.lastFrameMs;
  state.lastFrameMs = nowMs;
  // advance simulated time: speed x real time
  const advanceNs = dtMs * 1e6 * state.speed;
  let t = state.cursorTime + advanceNs;
  const end = state.times[state.times.length - 1];
  if (t >= end) {
    t = end;
    setCursorTime(t);
    togglePlay(); // stop at the end
    return;
  }
  setCursorTime(t);
  state.rafId = requestAnimationFrame(playStep);
}

// -------------------------------------------------------------------------- keyboard
function onKey(e) {
  if (e.target && (e.target.tagName === "INPUT" || e.target.tagName === "SELECT")) return;
  const idx = binarySearchTime(state.times, state.cursorTime);
  if (e.code === "Space") {
    e.preventDefault();
    togglePlay();
  } else if (e.code === "ArrowLeft") {
    e.preventDefault();
    stepIndex(idx, e.shiftKey ? -10 : -1);
  } else if (e.code === "ArrowRight") {
    e.preventDefault();
    stepIndex(idx, e.shiftKey ? 10 : 1);
  }
}
function stepIndex(idx, delta) {
  let n = Math.max(0, Math.min(state.times.length - 1, idx + delta));
  setCursorTime(state.times[n]);
}

// -------------------------------------------------------------------------- theme
function setTheme(dark) {
  state.dark = dark;
  document.documentElement.setAttribute("data-theme", dark ? "dark" : "light");
  el("btn-theme").textContent = dark ? "Light" : "Dark";
  el("btn-theme").setAttribute("aria-pressed", String(dark));
  if (state.heatmap) state.heatmap.setTheme(pal().surface, dark);
  if (state.meta) {
    buildCharts();
    render();
  }
}

// -------------------------------------------------------------------------- render
function render() {
  drawHeatmap();
  drawCursorOnHeatmap();
  const idx = binarySearchTime(state.times, state.cursorTime);
  updateLadder(idx);
  refreshOpenTableViews(idx);
}

// -------------------------------------------------------------------------- status
function setStatus(msg) {
  const s = el("status");
  s.textContent = msg;
  s.classList.remove("error", "hidden");
}
function setError(msg) {
  const s = el("status");
  s.textContent = "Error: " + msg;
  s.classList.add("error");
  s.classList.remove("hidden");
}
function clearStatus() {
  el("status").classList.add("hidden");
}

// -------------------------------------------------------------------------- wiring
function wire() {
  el("btn-open").addEventListener("click", () => el("file-input").click());
  el("file-input").addEventListener("change", (e) => {
    loadFromFiles(e.target.files).catch((err) => setError(err.message));
  });
  el("btn-play").addEventListener("click", togglePlay);
  el("btn-theme").addEventListener("click", () => setTheme(!state.dark));
  el("speed-select").addEventListener("change", (e) => { state.speed = Number(e.target.value); });
  el("time-slider").addEventListener("input", (e) => {
    const idx = Number(e.target.value);
    if (state.times) setCursorTime(state.times[idx]);
  });
  el("jump-time").addEventListener("change", (e) => {
    const t = parseClock(e.target.value);
    if (t != null && state.times) setCursorTime(state.times[0] + t);
  });

  document.querySelectorAll(".table-toggle").forEach((btn) => {
    btn.addEventListener("click", () => {
      const panel = btn.getAttribute("data-panel");
      const div = document.querySelector(`.table-view[data-panel-table="${panel}"]`);
      const show = div.classList.contains("hidden");
      div.classList.toggle("hidden", !show);
      btn.setAttribute("aria-pressed", String(show));
      if (show && state.times) renderTableView(panel, binarySearchTime(state.times, state.cursorTime), div);
    });
  });

  // heatmap hover -> shared cursor + tooltip
  const hc = el("heatmap-canvas");
  hc.addEventListener("mousemove", onHeatmapHover);
  hc.addEventListener("click", onHeatmapHover);

  window.addEventListener("keydown", onKey);
  window.addEventListener("resize", () => {
    if (state.meta) { buildHeatmap(); render(); }
  });
}

function onHeatmapHover(e) {
  const hv = state.heatmap;
  if (!hv) return;
  const rect = hv.canvas.getBoundingClientRect();
  const x = e.clientX - rect.left;
  const frac = x / rect.width;
  const span = hv.col1 - hv.col0 + 1;
  const col = hv.col0 + frac * span;
  const hm = state.meta.heatmap;
  const tNs = hm.t0_ns + col * hm.dt_ns;

  const y = e.clientY - rect.top;

  // Prefer a trade-dot tooltip when the pointer is near a dot (spec wants a distinct
  // time/price/qty/aggressor tooltip for trades); otherwise fall back to the cell tooltip.
  const hit = nearestTradeDot(x, y, rect.width, rect.height);
  if (hit) {
    const buy = state.views.trade_aggressor[hit.i] === 0;
    hv.canvas.title =
      `trade  t=${formatTime(state.views.trade_time_ns[hit.i] - state.times[0])}` +
      `  price=${formatPrice(state.views.trade_price[hit.i], state.meta.tick_display)}` +
      `  qty=${formatInt(state.views.trade_qty[hit.i])}  ${buy ? "BUY" : "SELL"}`;
  } else {
    // tooltip: time, price, size at the hovered cell
    const row = Math.floor((y / rect.height) * hm.rows);
    const price = hm.price_hi - row;
    const code = state.views.heatmap[Math.max(0, Math.min(hm.cols - 1, Math.round(col))) * hm.rows + Math.max(0, Math.min(hm.rows - 1, row))];
    hv.canvas.title = `t=${formatTime(tNs - state.times[0])}  price=${formatPrice(price, state.meta.tick_display)}  size≈${codeToSizeLabel(code)}`;
  }
  setCursorTime(tNs);
}

// Find the nearest trade dot to a canvas pixel (x,y), within a hit radius that follows the
// dot's drawn size. Maps trade time/price into the same pixel space the heatmap overlay
// uses (HeatmapView.timeToX / priceToY are canvas-backing-store coords, so scale by the
// displayed rect). Returns {i, dist} or null. Only visible-window trades are considered.
function nearestTradeDot(x, y, dispW, dispH) {
  const hv = state.heatmap;
  const V = state.views;
  const t = V.trade_time_ns;
  const n = t.length;
  if (!n) return null;
  const sx = dispW / hv.canvas.width;
  const sy = dispH / hv.canvas.height;
  const t0 = colToTimeNs(hv.col0, hv.meta.heatmap.t0_ns, hv.meta.heatmap.dt_ns);
  const t1 = colToTimeNs(hv.col1 + 1, hv.meta.heatmap.t0_ns, hv.meta.heatmap.dt_ns);
  let best = null;
  for (let i = 0; i < n; i++) {
    const tt = t[i];
    if (tt < t0 || tt > t1) continue;
    const dx = hv.timeToX(tt, hv.canvas.width) * sx - x;
    const dy = hv.priceToY(V.trade_price[i], hv.canvas.height) * sy - y;
    const dist = Math.hypot(dx, dy);
    // hit radius = drawn radius (+2px slack) in displayed pixels.
    const r = hv.dotRadius(V.trade_qty[i], state.qtyCap) * ((sx + sy) / 2) + 2;
    if (dist <= r && (!best || dist < best.dist)) best = { i, dist };
  }
  return best;
}

function codeToSizeLabel(code) {
  const vMax = state.meta.heatmap.v_max;
  const s = code <= 0 ? 0 : Math.expm1((code / 255) * Math.log1p(vMax));
  return s >= 10 ? s.toFixed(0) : s.toFixed(1);
}

function parseClock(str) {
  str = String(str).trim();
  if (!str) return null;
  // accept mm:ss(.mmm), h:mm:ss(.mmm), or plain seconds
  const parts = str.split(":").map((p) => p.trim());
  let sec = 0;
  if (parts.length === 1) sec = parseFloat(parts[0]);
  else if (parts.length === 2) sec = parseInt(parts[0], 10) * 60 + parseFloat(parts[1]);
  else if (parts.length === 3) sec = parseInt(parts[0], 10) * 3600 + parseInt(parts[1], 10) * 60 + parseFloat(parts[2]);
  else return null;
  if (!Number.isFinite(sec)) return null;
  return sec * 1e9;
}

// -------------------------------------------------------------------------- boot
function boot() {
  // Respect saved / OS theme but never start playback automatically.
  const prefersDark = window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches;
  setTheme(prefersDark);
  wire();
  const params = new URLSearchParams(location.search);
  const run = params.get("run");
  if (run) {
    loadFromRunParam(run).catch((err) => setError(err.message));
  } else {
    setStatus("No run loaded. Use ?run=<name> or the “Open run…” button (works from file://).");
  }
}

boot();
