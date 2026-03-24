# GitHub Pages site source

This directory contains the static site source files that the GitHub Actions
workflow publishes to the repository's `gh-pages` branch.

Current structure:

- `index.html` is the top-level landing page.
- `playground/` is the Emscripten playground section.
- `api/` is the API references section.

The intended published URLs are:

- `/` for the site landing page
- `/playground/` for the playground landing page
- `/playground/latest/` for the current playable Emscripten build
- `/api/` for the API references landing page
- `/api/latest/` for the current API reference placeholder

The GitHub Actions workflow at `.github/workflows/pages.yml` is responsible for
building the Emscripten artifacts and publishing this site tree to the
repository's `gh-pages` branch.

## Publishing notes

To make `/playground/latest/` actually runnable, copy the latest Emscripten output
from `cmake-build-wasm/source/tools/uapmd-app/` into `playground/latest/` in the
published site tree. At minimum, this normally includes `uapmd-app.js`,
`uapmd-app.wasm`, and any additional files emitted next to them by Emscripten.

The web runtime support files from `source/tools/uapmd-app/web/` should also be
present there when needed by the build:

- `coop-coep-sw.js`
- `audioworklet-env-fix.js`
- `uapmd-webclap-worklet.js`

API references are intentionally placeholders for now. The URL shape is already
version-oriented so that future docs can be published under paths such as
`/api/v0.3.0/` without changing the main landing pages.
