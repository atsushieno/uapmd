# Remidy Plugin Host - Web Example

This sample ports the `remidy-plugin-host` app to JavaScript by exposing the
@remidy/node bindings through a minimal HTTP server and a browser-based UI.

The Node.js process performs plugin discovery/instantiation and the frontend
communicates with it via REST calls. This lets you browse available plugins,
load one instance, inspect parameters, and toggle processing without leaving
the browser.

## Prerequisites

1. Build the native bridge (`js/build-native.sh`).
2. Install dependencies from the `js` folder:
   ```bash
   npm install
   npm run build
   ```

## Running the example

```bash
# from the js directory
npm run example:web-host
```

Then open <http://localhost:5173> in your browser. Use the **Scan Plugins**
button to populate the catalog (or load from the cache if available), select a
plugin to instantiate it, and flip the processing toggle to start or stop audio.

The server exposes the following endpoints:

- `GET /api/state` – Current catalog, instance, and parameter snapshot.
- `POST /api/scan` – Trigger a fresh plugin scan (saves cache under
  `~/.remidy/plugin-cache.json`).
- `POST /api/instance { pluginIndex }` – Instantiate a plugin by index.
- `POST /api/instance/start|stop` – Toggle processing.
- `POST /api/instance/parameter { id, value }` – Update a parameter value.

Press `Ctrl+C` in the terminal to stop the server; it will clean up the plugin
instance automatically.
