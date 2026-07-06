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
- `/playground/<tag>/` for archived playable Emscripten builds
- `/api/` for the API references landing page
- `/api/latest/` for the current Doxygen API reference set
- `/api/<tag>/` for archived Doxygen API reference sets

The GitHub Actions workflow at `.github/workflows/pages.yml` is responsible for
building the Emscripten artifacts and publishing this site tree to the
repository's `gh-pages` branch.

Only the landing/index pages come from `pages-src/`. The actual playable app
published under `/playground/latest/` and `/playground/<tag>/` is copied from
the build output as-is, including its own `index.html` and nested runtime files.

## Publishing notes

To make `/playground/latest/` actually runnable, publish the full Emscripten
output tree from `cmake-build-wasm/source/tools/uapmd-app/` into the site. Do
not replace it with a hand-written wrapper page.

Both playground builds and API references use the same version-oriented URL
shape so that generated artifacts can be published under paths such as
`/playground/0.5.0/` and `/api/0.5.0/` without changing the main landing pages.
