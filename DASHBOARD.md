# The lobsim replay dashboard

The dashboard replays one simulator run as a single synchronized timeline: an
order-book heatmap, a depth ladder, a price chart (mid, fundamental, MM quotes and
trades), the market maker's inventory, and the market maker's PnL. One shared cursor
and one time axis tie every panel together, with play, pause and scrub controls. It is
a no-build static site (plain HTML + CSS + ES modules, one vendored dependency,
[uPlot](https://github.com/leeoniya/uPlot)); the web app itself lives under
[`web/`](web/) and the on-disk bundle format it reads is documented in
[`viz/BUNDLE.md`](viz/BUNDLE.md).

## Panels and how to read them

### Heatmap (order-book liquidity over time)

The heatmap is the main panel: time runs left to right, price runs top to bottom (row
0 is the highest price, each row one tick below the last), and the colour of each cell
is the resting size at that price and time on a one-hue blue log scale. Darker/stronger
blue means more resting quantity. Each column is one time bucket (the heatmap sample
period, coarser than the ladder period) and the size shown is time-weighted over that
bucket, so a level that was large only briefly reads lighter than one that stood large
for the whole bucket. The best bid and best ask are drawn as thin overlay lines on top,
and trades are drawn as dots; the colour legend is labelled in **actual sizes** (lots),
decoded from the stored `v_max` through the inverse of the log encoding, not in raw
0–255 codes. The heatmap is pre-rendered once per zoom level into an offscreen canvas
and blitted each frame, so playback does not redraw it cell by cell.

### Depth ladder (top-of-book snapshot at the cursor)

The ladder is an HTML table for the cursor time: one row per price level, with the
price, the bid size and the ask size, and a thin horizontal bar in each size cell whose
length is proportional to the size so you can read relative depth at a glance. The bid
and ask columns use the buy and sell palette colours, but colour is never the only cue.
Where the market maker is quoting, the resting MM bid and MM ask levels carry a **text
label** (`MM bid` / `MM ask`) so they are identifiable without relying on colour. When
a run has no market maker these marks are absent.

### MM charts (inventory and PnL)

Two single-series uPlot charts track the market maker over the same timeline. The
**inventory** chart is signed position in lots, drawn against a zero baseline so you can
see at a glance when the MM is long (above zero) or short (below zero). The **PnL**
chart is cumulative mark-to-market profit and loss in price units, also against a zero
baseline. Neither uses a dual y-axis. Both panels are hidden entirely for a run with no
market maker (see Limits below), because their series are then all-missing.

## Exporting a run

A viz bundle is produced from a finished simulator run by `lobsim_viz_export`. First
produce a run:

```sh
build/lobsim_run --config configs/default.json [--seed N] [--duration-s X] --out <sim-out-dir>
```

Then export it into a bundle:

```sh
build/viz/lobsim_viz_export --run <sim-out-dir> --out <bundle-dir> \
    [--levels 10] [--ladder-dt-ms 100] [--heatmap-dt-ms 250] \
    [--heatmap-pad-ticks 30] [--budget-mb 8]
```

The exporter rebuilds the order book from `<sim-out-dir>/orders.log` exactly as the
replayer does, verifies the recomputed engine event hash and the recomputed MM PnL
against `<sim-out-dir>/summary.json`, and only then writes `<bundle-dir>/meta.json` and
`<bundle-dir>/data.bin`. It exits `0` on success and `2` on any verification failure or
bad input (and writes no bundle in that case). The flags control the number of depth
levels per side (`--levels`), the ladder and heatmap sample periods in milliseconds
(`--ladder-dt-ms`, `--heatmap-dt-ms`), how many ticks of padding the heatmap keeps above
and below the touched price range (`--heatmap-pad-ticks`), and the bundle size target
(`--budget-mb`). The full field-by-field format is in [`viz/BUNDLE.md`](viz/BUNDLE.md).

## Viewing a run locally

Two ways, no build step either way:

- **File picker (works from `file://`).** Open `web/index.html` directly in a browser,
  click **Open run…**, and select the run's `meta.json` and `data.bin`. No server
  needed.
- **Local server.** From the `web/` directory run `python3 -m http.server` and open
  <http://localhost:8000/?run=NAME>, which fetches `web/runs/NAME/meta.json` and
  `web/runs/NAME/data.bin`. Place bundles under `web/runs/<name>/` (that directory is
  for local testing and is not committed).

See [`web/README.md`](web/README.md) for the full list of controls and keyboard
shortcuts.

## Bundle size budget

The exporter targets roughly 8 MB per bundle by default (`--budget-mb 8`). When a run
would exceed the target it coarsens the sampling to fit: the heatmap and ladder sample
periods are widened, and the heatmap is **time-weighted downsampled** so that a wider
column still reflects how long each level held its size rather than just its value at a
single instant. Raising `--budget-mb` keeps finer sampling at the cost of a larger
`data.bin`; lowering it produces a smaller, coarser bundle.

## Publishing to GitHub Pages

The repository ships a GitHub Actions workflow
([`.github/workflows/pages.yml`](.github/workflows/pages.yml)) that builds the exporter,
runs the default seed-42 simulation, exports it into `site/runs/demo/`, copies the web
app into `site/`, and deploys the result to GitHub Pages. To enable it:

1. The repository must be **public** or on a plan that allows Pages.
2. In **Settings → Pages**, set **Source** to **GitHub Actions**.
3. Deploys run only from the **default branch**, so nothing publishes until the
   dashboard branch is merged into `main`. After the first successful deploy, the site
   URL appears in the workflow run summary.

`scripts/build_site.sh` reproduces the workflow's build steps locally if you want to
preview the published layout before merging.

## Limits

- **One instrument.** The dashboard shows a single order book, matching the simulator.
- **Simulated data only.** Every run is from lobsim's own simulator; no real market
  data feeds the dashboard.
- **Time-weighted downsampling in the heatmap.** Each heatmap column aggregates a time
  bucket (coarser than the ladder), so brief spikes in resting size are averaged over
  the bucket rather than shown at full instantaneous resolution. For exact per-sample
  values, read the ladder or switch a panel to its table view.
