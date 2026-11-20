# Vendored uPlot

uPlot is vendored (committed) so the dashboard is a no-build static site with no
runtime fetch from a CDN or package registry.

- Version: **1.6.31** (pinned)
- Source: https://github.com/leeoniya/uPlot
- License: MIT (see `LICENSE` in this directory)

## Files

| File | Source path in the upstream repo |
|---|---|
| `uPlot.iife.min.js` | `dist/uPlot.iife.min.js` |
| `uPlot.min.css` | `dist/uPlot.min.css` |
| `LICENSE` | `LICENSE` |

## Reproduce

```sh
git clone --depth 1 --branch 1.6.31 https://github.com/leeoniya/uPlot.git
cp uPlot/dist/uPlot.iife.min.js uPlot/dist/uPlot.min.css uPlot/LICENSE web/vendor/uplot/
```

The direct release-download URL (codeload) and the npm registry are blocked in the
build sandbox, so the files are obtained by a shallow git clone of the pinned tag,
not by `curl` or `npm`. Do not commit the clone's `.git` or full tree; copy only
the three files above.
