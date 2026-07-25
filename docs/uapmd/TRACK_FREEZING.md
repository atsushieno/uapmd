# Track Freezing

## Status

The first implementation is intentionally limited to explicit, manual
freezing. Automatic freezing and persistent rendered-audio caches are deferred
until the basic render, playback, and transport behavior is reliable.

## Initial objective

Each regular timeline track has an `Off` / `On` switch:

- `Off` processes the configured track graph normally.
- Switching to `On` snapshots and renders the track asynchronously.
- The live graph remains active until the complete render is published.
- Once published, realtime playback substitutes the rendered audio before the
  graph and therefore skips the track's plugin processing.
- Switching to `Off` immediately revokes the render and restores live graph
  processing.

The master track is not freezeable. Tracks with live device input are rejected
because their output cannot be reproduced from project data.

The switch is project data, keyed by the track's persistent reference ID.
Rendered audio is session-local derived data and is never stored in the project.
An `On` track is rendered again after project load.

## Runtime states

The manager reports four runtime states independently of the switch:

| State | Meaning |
| --- | --- |
| `Live` | The normal track graph is processing. |
| `Rendering` | An isolated render is in progress; the live graph remains active. |
| `Frozen` | Immutable rendered audio is published to realtime playback. |
| `Error` | Rendering failed; the live graph remains active. |

An output-affecting edit increments the track's generation, revokes the
published audio, and starts a new render if the switch is still `On`. A render
may publish only when its generation is still current.

## Isolation

The renderer must never borrow the realtime engine's transport, processing
contexts, plugin instances, latency buffers, or pump rings.

For the initial implementation, a freeze request serializes only the selected
track into a temporary project snapshot. The snapshot is loaded into a second
`SequencerEngine` with track freezing disabled. That isolated engine owns the
plugin instances and all mutable DSP and transport state used by the offline
render. The temporary project is removed after completion.

This is intentionally more expensive than a future in-memory track-cloning API,
but it reuses the established project/plugin-state loading path and establishes
the required isolation boundary.

## Realtime substitution contract

`FrozenTrackAudioProcessorExtension` is invoked immediately before the normal
track graph. It opts in only for a valid `Frozen` result.

The callback:

- performs no locking, allocation, file I/O, or mutable project access;
- clears every output block before copying cached samples;
- writes zero for channels or timeline ranges not covered by the cache;
- writes silence while playback is paused or stopped;
- indexes the cache using the engine's render playback position;
- leaves normal routing, output alignment, mixing, and master processing intact.

Playback state and immutable audio are published through atomic pointers.
Published objects are retained for the manager's lifetime so the audio thread
never participates in reclamation.

Before a frozen track returns to live processing, its suspended plugin
processing state and host-side track buffers are reset while the audio callback
is excluded from that track transition. This prevents old delay lines, effect
tails, queued events, or pump buffers from resuming after unfreeze.

## Memory policy

The first implementation stores immutable float PCM in memory and applies a
fixed 512 MB per-track limit. A render exceeding the limit fails safely and the
track remains live.

Disk cache identity, cache reuse, user-configurable budgets, eviction, and
cross-session validation are deferred.

## Deferred work

- automatic freeze timing and policy;
- cache directory, fingerprints, index, size setting, and eviction;
- partial/timeline-range rendering;
- checkpointed or playback-priority rendering;
- polished progress/error overlays;
- broader unsupported-source detection;
- a direct in-memory isolated track snapshot API.

## Validation checklist

- [x] Project serialization extension and persistent track IDs.
- [x] Optional pre-graph audio-processor extension seam.
- [x] Manual `Off` / `On` track control.
- [x] Generation-based invalidation.
- [x] Plugin parameter invalidation notification.
- [x] Isolated project snapshot and render-engine construction.
- [x] Session-local immutable PCM publication.
- [x] Realtime cached-audio substitution.
- [ ] Validate freeze/unfreeze during playback.
- [ ] Validate pause, resume, stop, restart, and seek.
- [ ] Validate edit-during-render and repeated toggle races.
- [ ] Validate plugin latency and effect tails.
- [ ] Validate project load/save on supported desktop platforms.
- [ ] Validate Android audio-thread behavior.
