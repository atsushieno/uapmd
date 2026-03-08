# WebCLAP Integration Notes (Temp)

_Last updated: 2026-03-08T16:10:33Z_

## Current Status
- COOP/COEP headers plus the gesture gate keep SAB/SharedArrayBuffer access stable in Chromium/Firefox. Browser builds now start with the audio engine disabled and default to a 1024-frame buffer so WebCLAP processing can opt into larger blocks.
- The stub manifest enumerates the two published WebCLAP archives, catalog + scanning wire them into PluginFormatWebCLAP, and instances now instantiate end-to-end (host extensions still stubbed).
- JS bridge + wasm exports agree on table/memory wiring; the Playwright harness reaches `requestInstance`, but processing is still stubbed and we intentionally drop frames with `[WebCLAP] processInstance not wired yet`.
- Work is paused until we stand up an isolated “audio engine process” (headless Wasm/worker + IPC). WebCLAP audio work will resume only after that split is in place.

## TODO / Next Focus
1. Define and implement the isolated audio-engine process + IPC layer for the browser build (prerequisite for any further WebCLAP progress).
2. Once the engine split exists, resume WebCLAP work: wire up `process` bridging, implement host extensions (`clap.state`, `clap.audio-ports`, `clap.params`, `clap.gui`), and add actionable error logging.
