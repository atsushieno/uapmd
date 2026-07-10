# Latency Compensation (AI slop)

## Scope

This document describes the latency-compensation functionality that is already
implemented in `uapmd-engine` and surfaced in `uapmd-app`.

This is design documentation for the current implementation.
It is not a task list.
For remaining work and product gaps, see `LATENCY_COMPENSATION_PLAN.md` at the
repository root.

## Goals

The implemented latency-compensation stack currently aims to provide:

- compensated playback against plugin-reported latency
- compensated offline export without leading silence
- output-path alignment when different buses have different downstream latency
- a first-cut monitoring model that can trade phase correctness for lower live-input latency

It does not yet claim full DAW-grade completeness for dynamic latency changes,
complex downstream routing, or all timeline corner cases.

## Architectural split

Latency compensation is intentionally split across layers.

- `remidy::PluginInstance` and `uapmd::AudioPluginInstanceAPI`
  - own plugin-reported latency and tail primitives
- `AudioPluginGraph`
  - owns graph-local interpretation of latency and tail
- `uapmd-engine` sequencer layer
  - owns preroll, render-lead, stop-drain, output holdback, and monitoring policy
- `uapmd-app`
  - owns the current user-facing and debug-facing control surfaces

This split is important:

- plugin APIs report facts
- graph APIs summarize graph-local timing
- the sequencer decides transport behavior

## Timing primitives

### Plugin layer

The plugin layer exposes:

- `latencyInSamples()`
- `tailLengthInSeconds()`

Latency remains sample-domain because scheduling and alignment are sample-domain.
Tail remains time-domain because it is a duration policy, not a stable sample identity.

### Graph layer

The graph layer exposes:

- `mainOutputLatencyInSamples()`
- `mainOutputTailLengthInSeconds()`
- `outputBusCount()`
- `outputLatencyInSamples(outputBusIndex)`
- `outputTailLengthInSeconds(outputBusIndex)`
- `renderLeadInSamples()`

The simple linear graph implements these as scalar-chain behavior.
The full DAG graph computes them from compiled graph connections.

`renderLeadInSamples()` is currently a practical scheduling value.
It is not yet a fully route-aware audible-path model.

### Sequencer layer

The sequencer exposes timing inspection at track and master levels:

- `SequencerTrack::latencyInSamples()`
- `SequencerTrack::renderLeadInSamples()`
- `SequencerTrack::tailLengthInSeconds()`
- `SequencerEngine::trackLatencyInSamples()`
- `SequencerEngine::masterTrackLatencyInSamples()`
- `SequencerEngine::trackRenderLeadInSamples()`
- `SequencerEngine::masterTrackRenderLeadInSamples()`

These values are used by preroll, stop-drain, output alignment, Mixer Monitor,
and offline render.

## Playback model

The engine distinguishes two transport positions:

- audible playback position
- render playback position

The audible position is what the user perceives as the transport position.
The render position may start earlier so that upstream plugin latency is already
filled by the time audible playback reaches the requested position.

This is the basis of hidden preroll.

### Preroll

When playback starts, resumes, or seeks:

- the audible position is set to the requested timeline position
- the render position is moved earlier by the required preroll amount
- timeline pumping and graph processing run from the render position
- audible output is not emitted until the render position catches up

This allows:

- latency-compensated startup
- latency-compensated seek
- latency-compensated resume

The same model is also used for offline rendering.

### Stop drain

When transport stops, the engine may keep processing for a bounded drain period.
This lets pending delayed audio and finite plugin tails reach the output.

The implemented drain calculation now uses one shared internal model for:

- realtime stop-drain
- offline post-range drain

Both paths evaluate:

- render lead
- downstream path latency
- track tail
- downstream tail

They intentionally differ only in fallback policy for infinite tails.

## Output alignment

Latency compensation does not stop at preroll.
When a track has multiple effective output paths with different downstream
latency, the faster paths must be held back so they remain aligned at the
destination.

The current implementation provides:

- per-output-bus path latency calculation
- per-output-bus holdback calculation
- preallocated delay lines outside the audio-thread hot path
- per-track and per-bus inspection APIs

The holdback is applied before track outputs are mixed into master or main mix.

This is currently implemented with the routing model that exists today:

- bus outputs can route toward master input buses
- or toward the main mix path

It is functional, but still narrower than a full DAW mixer/routing design.

## Monitoring policy model

The current user-facing model has two project-level policy selectors and two
per-track states.

### Project-level policies

- `Playback Compensation`
  - `Compensated`
  - `Low-Latency`
- `Input Monitoring`
  - `Tape Style`
  - `Auto`
  - `Off`

### Per-track states

- record arm
- monitor enable

### Semantics

`Playback Compensation = Compensated`

- normal playback uses full latency compensation
- non-monitored paths stay aligned and compensated

`Playback Compensation = Low-Latency`

- monitored live-input behavior prefers responsiveness over full alignment

`Input Monitoring = Tape Style`

- a track uses immediate monitored live input only when it is both:
  - record-armed
  - monitor-enabled

`Input Monitoring = Auto`

- a track uses low-latency monitoring when it has live input and is monitor-enabled
- other playback remains compensated

`Input Monitoring = Off`

- live-input monitoring is disabled through the compensation layer

### Internal mapping

Internally the current implementation still uses:

- `PlaybackCompensationMode`
- `InputMonitoringPolicy`
- `OutputAlignmentMonitoringPolicy`

These internal types are implementation details of the current compensation stack.

## Infinite-tail handling

Infinite tails are handled differently in realtime and offline contexts.

### Realtime

Realtime stop behavior uses `RealtimeInfiniteTailPolicy`:

- `LATENCY_FALLBACK`
- `IMMEDIATE_STOP`

This is intentionally treated as debug/runtime state, not stable project policy.

### Offline

Offline render exposes export-only behavior:

- guard duration
- optional silence-stop
- infinite-tail fallback mode

Offline export is still compensated by default.
Its extra controls exist because export can afford policies that are not suitable
for interactive transport behavior.

## Timeline integration

Latency compensation depends on the timeline layer being able to pump sources
correctly around sub-block boundaries and hidden preroll.

The current implementation includes:

- compensated loop-wrap pumping
- sample-accurate overlap handling for clips
- warped audio clip rendering inside the timeline source layer
- offline preroll rendering and discard
- bounded compensated post-range drain

This means warped audio clips already participate in the compensation model.

## Project persistence

The current stable policy state is persisted inside:

- `settings.latency_compensation`

The default implementation persists:

- `implementation`
- `playback_compensation_mode`
- `input_monitoring_policy`
- `monitored_tracks`
- `record_armed_tracks`
- implementation-owned `properties`

This is intentionally designed as an extensibility point.
Project save/load does not hard-code every future compensation detail into the
top-level project schema.

## Control surfaces

### GUI

The current user-facing GUI surface is the `Mixer Monitor` window.

It currently shows:

- playback compensation mode
- input monitoring mode
- per-track record arm
- per-track monitor enable
- latency, render lead, holdback, tail, and route inspection

It also contains debug-only controls, explicitly labeled as debug.

### JavaScript

The JavaScript runtime exposes two separate surfaces:

- stable policy surface
  - `getLatencyCompensationState()`
  - `setLatencyCompensationState(payload)`
- debug surface
  - `getLatencyCompensationDebugState()`
  - `setLatencyCompensationDebugState(payload)`

The stable surface covers only:

- playback compensation mode
- input monitoring mode
- per-track record arm
- per-track monitor enable

Debug runtime toggles are intentionally excluded from that stable payload.

### MCP

The MCP server mirrors the same split:

- stable policy tools
  - `get_latency_compensation_state`
  - `set_latency_compensation_state`
- debug tools
  - `get_latency_compensation_debug_state`
  - `set_latency_compensation_debug_state`

This separation exists so that project-facing automation does not accidentally
depend on debug transport behavior.

## Implementation notes

### Routing and compensation ownership

Track-routing interpretation lives in the routing manager.
Latency policy state lives in the latency-compensation manager.
Sequencer transport uses both, but the newer design direction is to keep policy
and routing details out of `SequencerEngine` as much as practical.

### Shared stop-drain computation

Realtime and offline drain now use one shared internal stop-drain computation.
This reduces divergence between:

- transport stop behavior
- export tail capture behavior

The two paths still keep intentionally different policy entry points.

### Realtime safety

The compensation layer is designed to avoid allocation-heavy or blocking work in
the audio-thread hot path.

Current measures include:

- preallocated output-alignment delay lines
- cached routing-derived timing values
- mutation-driven reconfiguration outside steady-state processing

This does not mean every related code path is complete or optimal yet, but it is
the intended design rule.

## Non-goals of this document

This document does not define:

- future DAW-grade routing behavior
- future runtime latency-change convergence behavior
- final placement of all product UI
- complete validation coverage for complex song projects

Those belong to the ongoing plan and future design work, not to the description
of the currently implemented system.
