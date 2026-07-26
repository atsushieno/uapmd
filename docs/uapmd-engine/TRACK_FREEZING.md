# Track Freezing (AI slop)

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

## Transport restriction

Track freezing is never performed while realtime playback is active.

- The user may change the `Off` / `On` switch at any time.
- Switching to `On` during playback records the policy immediately, but its
  render is deferred until playback stops or pauses and every track has passed
  its declared tail length.
- Switching to `Off` during playback immediately cancels a deferred or active
  render and restores live processing.
- A render may start only while playback is inactive.
- Starting or resuming playback invalidates and cancels any render in progress.
  Its output is discarded, the existing plugin instances are restored, and the
  explicitly requested transport operation begins as soon as restoration is
  complete. The `On` policy remains pending and freezing restarts from the
  beginning after playback stops or pauses.
- A completed render may not be published after playback has started.
- Freeze completion by itself never starts or resumes playback. Only a pending,
  explicit user transport request may do so after canceling a render.
- Stop and Pause use the same tail-drain path and differ only in their final
  position: Stop targets the beginning, while Pause retains its current
  position.
- `TailProcessManager` owns the stopped-transport drain and quiet-detection
  state. `SequencerEngine` tells it when transport starts or stops, supplies
  the drain length calculated by `LatencyCompensationManager`, and advances it
  from audio processing. The two managers do not depend on each other.
- `TailProcessManager` raises a transport-quiet event only after all track and
  master declared tails have drained and the mixed output has remained below
  -80 dB for 250 ms. The silence observation is required because instruments
  may report a zero tail while note-release audio is still audible. The
  realtime thread publishes that transition without locking or allocation;
  listeners run off the realtime thread.
- The freezer registers directly with `TailProcessManager` for the
  transport-quiet event. Stopping or pausing marks a render as pending but
  does not start it; the event starts pending renders only if playback has not
  subsequently started or resumed.
- A render session defensively normalizes its saved public transport state to
  stopped. Restoring that state cannot set `isPlaying`; an explicit pending
  Play or Resume request is the only operation allowed to do so afterward.
- The renderer's running transport is private plugin-processing state. It must
  not set the application's timeline or transport `isPlaying` state.
- That private running state also applies to timeline audio and MIDI source
  nodes. They must produce the same events and samples as realtime playback
  without changing the stopped public timeline.
- Invalidation that occurs during playback is remembered. The existing frozen
  result remains active for the rest of that playback unless the user switches
  it `Off`; otherwise it is revoked and rendered again after stop or pause.

This is an engine-level execution invariant, not a restriction on editing the
requested policy. All callers, project loading, edit invalidation, and
asynchronous render completion must obey it.

The switch is project data, keyed by the track's persistent reference ID.
Rendered audio is session-local derived data and is never stored in the project.
An `On` track is rendered again after project load.

## Runtime states

The manager reports four runtime states independently of the switch:

| State | Meaning |
| --- | --- |
| `Live` | The normal track graph is processing. |
| `Rendering` | The track is `Busy` while its existing plugin instances are being rendered. |
| `Frozen` | Immutable rendered audio is published to realtime playback. |
| `Error` | Rendering failed; the live graph remains active. |

An output-affecting edit increments the track's generation, revokes the
published audio, and starts a new render if the switch is still `On`. A render
may publish only when its generation is still current.

## Editing and Busy track access

The application must visibly label a track as `Busy` for the entire freeze
operation, including state capture, audio rendering, state restoration, and
buffer cleanup.

While audio rendering is in progress, the timeline minimap shows a dedicated
freeze-progress strip. It does not reuse or alter the stopped transport
position. The progress disappears when rendering completes, is canceled, or
fails.

Project edits are not prohibited merely because a track is `Frozen`. Every
track-aware operation that makes the project dirty revokes that track's
session-local cache while preserving its `On` policy. A fresh render starts
after the edit when transport is stopped, or is deferred until playback stops.

Dirty notification is part of the mutation contract and is emitted for every
edit, even when the project was already dirty. Clip, graph, routing, parameter,
preset, and plugin-state mutations must identify their affected track. A
mutation received while rendering invalidates the current generation; its
partial result cannot be published, and rendering restarts from the edited
state after existing-instance restoration.

Realtime UMP delivery to a `Busy` track remains prohibited. Operations that
cannot safely participate in the dirty mutation contract, including opening a
GUI backed by the plugin instance currently used by the renderer, also remain
unavailable until restoration completes. Read-only operations remain available.

The application and all other tracks remain responsive and usable. Rendering
is divided into bounded tasks on the main event loop so that the `Busy` label,
unfreeze cancellation, and unrelated work continue to be serviced between
render chunks. A freeze operation must never block the application event loop
for the duration of the track.

Switching freezing `Off` requests cancellation. The track remains `Busy` and
unavailable until cancellation has restored the existing plugin instances and
cleared all render state. It becomes editable again only after returning to
`Live`.

## Isolation

The initial implementation must not create another plugin instance for
freezing. Some plugins and their underlying systems do not support a second
instance reliably, so a second `SequencerEngine` loaded from a project snapshot
is prohibited even if it avoids sharing mutable DSP state.

Freezing must use the selected track's existing plugin instances. It must not
construct a plugin scanner, perform plugin discovery, reload or rebuild the
plugin catalog, or instantiate the track's plugins again.

Because the existing instances are reused, the render must be exclusive of
realtime playback and must explicitly control all mutable engine state involved
in the operation. In particular, the implementation must preserve or reset the
transport, plugin processing state, processing contexts, latency buffers, pump
rings, queued events, and remaining audio before the track can return to normal
playback. No partially rendered or residual audio may become observable.

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

When a freeze is pending, Stop and Pause still let the normal realtime path
emit the complete plugin tail. Rendering begins only after the transport-quiet
event, so render exclusion never suspends a tail and freeze completion cannot
release it later. Before render exclusion is released, all track, pump, mix,
master, alignment, event, and device-output state is cleared. Device output
remains hard-silent after freezing begins until an explicit Play or Resume
request. Freeze completion must never expose buffered, rendered, or remaining
audio to the audio device.

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
- [ ] Existing-instance render path with no additional plugin instantiation.
- [x] Session-local immutable PCM publication.
- [x] Realtime cached-audio substitution.
- [x] Engine-level prohibition on freezing during playback.
- [ ] Validate freeze/unfreeze during playback.
- [ ] Validate pause, resume, stop, restart, and seek.
- [ ] Validate edit-during-render and repeated toggle races.
- [ ] Validate plugin latency and effect tails.
- [ ] Validate project load/save on supported desktop platforms.
- [ ] Validate Android audio-thread behavior.
