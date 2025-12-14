# Remidy Plugin Host – Electron Example

This example shows how to build a desktop-style host around `@remidy/node`
using [Electron](https://www.electronjs.org/). The Electron main process
handles plugin scanning/instancing through the Node bindings, while the UI is
rendered with standard web technologies. When a plugin exposes a custom editor,
the sample spawns a native container window (via remidy-gui) so the editor is
shown just like in the native `remidy-plugin-host`.

## Prerequisites

1. Build the native bridge and TypeScript bindings:
   ```bash
   cd js
   npm install
   npm run build
   ```
2. Make sure the native remidy dependencies are built (run the usual CMake
   configure+build from the repository root beforehand).

## Running

```bash
npm run example:electron-host
```

On startup the app shows a catalog pane, lets you trigger a fresh scan, load a
plugin, inspect its parameters, and open/close the plugin-provided UI. The
plugin editors are real native windows, so UI resizing and focus behave the
same way they do in the C++ host. A `GLContextGuard` is used around plugin UI
operations to mirror the native host's protection against plugins that leave
the OpenGL context in an unexpected state.
