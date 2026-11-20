# lobsim replay dashboard (`web/`)

A no-build static site that replays a `lobsim` run: an order-book heatmap, a depth
ladder, price / fundamental / MM-quote / trade charts, and MM inventory and PnL, all
on one synchronized timeline with play, pause and scrub.

Plain HTML + CSS + ES modules. No framework, no build step, no bundler. The only
runtime dependency is [uPlot](https://github.com/leeoniya/uPlot), vendored (pinned)
under `vendor/uplot/` with its MIT license.

## Viewing a run

A run bundle is two files, `meta.json` and `data.bin`, produced by
`lobsim_viz_export` (see [`../viz/BUNDLE.md`](../viz/BUNDLE.md) for the format).

### Hosted / local server

From this `web/` directory:

```sh
python3 -m http.server
```

then open <http://localhost:8000/?run=NAME>, which fetches
`runs/NAME/meta.json` and `runs/NAME/data.bin`. Place bundles under `web/runs/<name>/`.
(`web/runs/` is for local testing and is not committed.)

### From `file://` (no server)

Open `index.html` directly, click **Open run…**, and select both `meta.json` and
`data.bin`. The file picker path works without a server.

## Controls

- **Play / Pause** (or **Space**) — playback never starts by itself.
- **Speed** — 1×, 10×, 60×, 600× of real time.
- **Time slider** and **Jump** box (`mm:ss`, `h:mm:ss`, or seconds).
- **←/→** step one sample, **Shift+←/→** step ten.
- **Dark / Light** toggle.
- **Table view** per panel shows the values for ±20 samples around the cursor, with
  buy/sell distinguished by text (colour is never the only cue).

Hovering or scrubbing any panel moves one shared cursor everywhere; dragging on a chart
zooms every panel (including the heatmap); double-click resets.

## Modules

| File | Role |
|---|---|
| `js/bundle.js` | Decode `meta.json` + `data.bin` into typed-array views (pure, node-importable). |
| `js/search.js` | Binary search over the sorted time arrays (pure). |
| `js/format.js` | Time / price formatters (pure). |
| `js/heatmap.js` | Blue log colour ramp, offscreen pre-render, overlays, legend. |
| `js/app.js` | Controller: loading, shared cursor, zoom, playback, keyboard, tables, theme. |

## Vendored uPlot

Pinned at `vendor/uplot/` (`uPlot.iife.min.js`, `uPlot.min.css`, `LICENSE`). The version
is recorded in `vendor/uplot/README.md`.
