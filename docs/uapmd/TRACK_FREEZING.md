# Track Freezing (AI slop)

## Status

Design and implementation in progress. The policy, project-extension, and
pre-graph extension foundations have landed; rendering and cache playback have
not yet been implemented.

This document records the intended behavior and implementation decisions for
uapmd-app. Update the progress checklist as work is completed, and update the
decision sections before changing a decided behavior.

## Goal

Avoid processing unchanged track plugin graphs in the realtime audio callback.
An eligible track is rendered asynchronously to cached audio, and realtime
playback substitutes that audio for the track graph. When a user needs to edit
the track, the app switches it back to live processing first.

## Terms and state model

Each regular timeline track has two independent pieces of state:

- **Freeze policy**: the user's selected three-state toggle: `Auto`, `On`, or
  `Off`.
- **Runtime freeze state**: `Live`, `Waiting`, `Rendering`, `Frozen`,
  `Unfreezing`, `Invalid`, `Error`, or `CachingUnavailable`.

The policy is a user preference; runtime state reports what the engine is
actually doing. Unfreezing preserves `Auto`, but changes `On` to `Off`.

### Policy semantics

| Policy | Behavior |
| --- | --- |
| `Auto` | Freeze after the auto-freeze timeout, provided the track is eligible and has a valid, unchanged render input. |
| `On` | Explicitly request freezing. Selecting it queues an asynchronous freeze immediately. |
| `Off` | Always process the track graph live. Any cached render may remain in the local cache, but is never used for playback. |

`On` is an explicit user action and does not wait for the auto-freeze timeout.

### Unfreezing

Clicking the overlay performs an asynchronous unfreeze operation. It switches
audio playback to the live graph and resets the track's accumulated live
duration.

- For `Auto`, the policy remains `Auto`, so the track becomes eligible to
  freeze again after the configured auto-freeze timeout.
- For `On`, the operation changes the policy to `Off`. The user must select
  `On` again to explicitly freeze the track again.
- For `Off`, it remains live.

## Automatic freezing

The Settings window contains **Auto Freeze minutes**, an integer slider in the
range `1..20`. An `Auto` track is queued for freezing after it has remained
unchanged for the configured number of minutes.

The timeout is measured from accumulated live duration, not elapsed wall-clock
time. Each audio-processing pass increases a live track's duration by its
processed frame count at the current sample rate. Changing a clip, a source,
plugin topology/state/parameter, or another input that changes the rendered
result invalidates the active render, returns the track to live processing, and
resets this duration. This makes the behavior independent of debugger pauses
and other pauses in application execution.

## Eligibility and scope

The first implementation applies only to regular timeline tracks. The master
track is excluded because it processes the mixed project output.

A track must remain live when it has device input or another nondeterministic
or unsupported live source. The engine's `trackHasLiveInput()` capability is
the initial eligibility check. Further unsupported plugin/input cases must
fail safe: retain live audio and show an explanatory runtime state rather than
producing an incomplete frozen result.

Freeze input invalidation must cover, at minimum:

- clip, source-node, and timeline edits;
- track plugin add/remove/reorder/connect/disconnect operations;
- plugin parameter and state changes;
- changes to graph settings that affect audio output;
- relevant track-routing or latency changes.

## Rendering and realtime substitution

Rendering, cache loading/maintenance, and freeze-state transitions run
asynchronously. Completed work is published to the playback path only when its
generation is still current.

`FrozenTrackManager` owns all track-freezing behavior: policy/runtime state,
cache lookup and maintenance, invalidation, rendering jobs, and the playback
substitution decision. Its two separately owned, public extensions are
`FrozenTrackManagerProjectSerializationExtension` and
`FrozenTrackAudioProcessorExtension`. Callers can unregister and re-register
either concrete extension with its respective non-owning registry; no core
project data class is extended.

The engine remains independent of track freezing. If `FrozenTrackManager` is
not created, disabled, or has no frozen output for a track, engine processing
is unchanged and every track graph runs normally. In particular, freezing must
not replace the track's graph: a track still needs its configured custom graph
when it is live.

Each track has a serialized operation queue. Operations are generation-tagged
with the track's current invalidation generation. A completion may publish its
result only if its generation is still current; stale work is discarded. This
prevents contradictory requests such as edit -> freeze -> edit -> unfreeze
from restoring an obsolete render.

The realtime engine needs one narrow optional connection point before it
invokes a track graph's `processAudio()`. It maintains an ordered list of
audio-processor extensions. For each track block, it asks each extension if it
should process the block; the first one that returns `true` processes it and
stops the iteration. If none opt in, the existing graph call is unchanged.
`FrozenTrackManager` is the only initial consumer: for a valid `Frozen` track,
it writes cached audio into the output context. Normal gain, routing, latency
compensation, master processing, and final mixing continue unchanged.

The existing `TrackOutputHandler` is not suitable: it is called after the
track graph has already processed, so it cannot reduce plugin work. It is also
used by the WebAudio implementation. The new connection point must therefore
be separate from that output-routing hook.

Recommended engine API shape:

```cpp
class TrackAudioProcessorExtension {
public:
    virtual bool shouldProcessAudio(
        SequencerEngine&, uapmd_track_index_t, SequencerTrack&,
        AudioProcessContext&) = 0;
    virtual void processAudio(
        SequencerEngine&, uapmd_track_index_t, SequencerTrack&,
        AudioProcessContext&) = 0;
};

virtual void addTrackAudioProcessorExtension(
    TrackAudioProcessorExtension&) = 0;
virtual void removeTrackAudioProcessorExtension(
    TrackAudioProcessorExtension&) = 0;
```

The engine invokes extensions immediately before `track.graph().processAudio`.
Both methods receive the owning `SequencerEngine`, so an extension can make a
practical per-track decision using engine state rather than only the
audio-process context. This is the only engine change currently required for
the first implementation.

The current project-wide offline renderer is not the implementation mechanism:
it owns transport state and writes a complete project WAV. Track freezing needs
a dedicated per-track renderer or isolated render engine that can run without
disrupting realtime playback.

There is no current public per-track offline-render API. `FrozenTrackManager`
must render through an isolated graph/engine instance rather than process the
active track graph concurrently with playback. This isolation should be built
from the existing graph serialization/instantiation facilities in app-model
where possible. If that proves insufficient, the only additional engine API to
consider is a narrowly-scoped helper for creating an isolated renderable track
instance; it must not make freezing state part of `SequencerTrack` or a graph
implementation.

### Deferred advanced design: playback-priority rendering

When a track becomes eligible for freezing while playback is already running,
we may prioritize rendering from the current render position rather than
starting by writing the beginning of the timeline. The intended result is to
make a frozen result available near the playhead first, then render the earlier
portion and retain both timeline-indexed regions as one logical cache entry.

This cannot simply begin processing at the current position. Stateful
instruments and effects depend on preceding MIDI, automation, sidechain input,
and effect/plugin state. Directly rendering from the playhead can therefore
produce audio that differs from normal playback. A correct implementation must
silently pre-roll the isolated renderer from a valid checkpoint (initially the
project start) before publishing audio at the prioritized position. Future
checkpoint support could reduce that work, but is not required initially.

Published results should be represented as timeline-indexed ranges, rather
than a physically concatenated render. A possible order is `[current, end)`
first, followed by `[start, current)`. If playback jumps backward into an
uncovered range, the simple correct behavior is to cancel current work and
restart from the earlier position. Preserving and scheduling existing fragments
is a later optimization. A forward jump may reuse an already-published range;
otherwise the track remains live until its range is ready.

This requires a discrete transport-discontinuity notification, not a listener
for every advancing audio-block playhead. `jumpPlayback()` already represents
an explicit seek and advances the transport generation; an eventual observer
should report the old and new render positions plus that generation, then
enqueue manager work asynchronously. It must not perform cache or render work
in the notification path.

This design is deferred. We will decide whether to implement it after the
isolated per-track renderer and basic cache playback are working.

## Project data and local cache

Freeze policy and settings are project data, but rendered audio is not.

Use `FrozenTrackManagerProjectSerializationExtension` (rather than changing
the core project data classes). Its versioned manifest stores:

- the project auto-freeze setting;
- a per-track policy, keyed by persistent track reference ID rather than track
  index;
- only durable freeze-related settings needed to restore the user experience.

The rendered audio and cache index live in app-local data. A cache entry is
validated with a content/input fingerprint, render format, and source project
identity. A missing, stale, or unreadable entry is a cache miss: playback stays
live and `Auto`/`On` may queue a new render. Project save and archive creation
must never copy cached audio into the project.

The Settings window also contains:

- **Rendering cache size**: displayed in MB (and backed by a configurable
  maximum cache budget).

The cache budget is an application-level preference; it is not part of a
project extension manifest. Cache validity is an internal policy and is not
user-configurable. It determines whether a local cached render remains
reusable or can be discarded.

Eviction runs asynchronously. On cache pressure, it first removes eligible
entries belonging to other projects according to the cache-validity policy. It
must not evict a file currently used by playback or a pending operation. If the
configured cache size cannot accommodate a new render after all eligible
other-project entries have been considered, track freezing becomes unavailable
for the project: all tracks stay live and the app reports a warning that
caching cannot be enabled. Stored per-track policies are retained, so the
feature can be retried after cache capacity becomes available.

## UI

- Rename **Device Settings** to **Settings**.
- Add the auto-freeze slider and cache controls to that window.
- Prepend a single cycling `Off` → `Auto` → `On` toggle to each regular
  track's **Add Plugin** control. It displays only the active mode, making
  automatic freezing the natural intermediate choice rather than making it
  easy to disable accidentally.
- When runtime state is `Frozen`, cover the timeline track with an interactive
  **Click to unfreeze** overlay. It represents runtime state, not the policy.
- While rendering or transitioning, show a non-interactive progress/status
  overlay. On failure, show a concise error and preserve live playback.

## Persistence and lifecycle

Project load applies the extension manifest after timeline tracks are created.
Tracks with no stored policy default to `Off`; freezing is opt-in. Cache lookup
for opted-in tracks is then performed asynchronously. Project load must not
block on cache I/O or rendering.

Track deletion removes its manifest entry and schedules its local entries for
eviction. Project save writes only the extension manifest. Cache cleanup is
best-effort and must never make saving fail.

## Implementation progress

- [x] Identify the project serialization extension mechanism.
- [x] Identify the realtime per-track graph-processing path and existing
  project-wide offline renderer.
- [x] Decide policy/runtime-state separation and unfreeze behavior.
- [x] Decide that rendered audio is app-local cache data, not project data.
- [x] Decide asynchronous, generation-aware per-track operation queues.
- [ ] Define the stable project/track cache identity and cache directory.
- [x] Implement the project serialization extension and register it.
- [x] Implement the Auto Freeze Minutes Settings control and manifest field.
- [x] Implement `FrozenTrackManager` policy ownership and track controls.
- [ ] Implement invalidation tracking and operation queues.
- [ ] Implement per-track offline rendering.
- [x] Add the optional pre-graph audio-processor extension seam in the engine.
- [ ] Implement cached-audio substitution through that seam.
- [ ] Implement cache index, cache-size setting, cache-validity policy, and
  asynchronous eviction/fallback warning.
- [ ] Implement timeline overlay, three-state control, and status/error UI.
- [ ] Validate race handling, project load/save behavior, and Android audio
  thread behavior.
