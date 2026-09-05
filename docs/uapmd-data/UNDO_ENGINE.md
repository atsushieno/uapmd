# Project Commands and Undo History (AI slop)

## Scope and status

The command and history infrastructure is implemented in `uapmd-data`.
`uapmd-engine` owns each project's history instance through `TimelineFacadeImpl`
and supplies the concrete sequencer mutations. The native application,
JavaScript and MCP clients share that project history. The scope is the project
document: clips, tracks, plug-in graphs, plug-in state and authored content.
Transport, rendering, scanning, cache contents and UI-window state are runtime
state and are not undoable.

The document mutation funnel, persistent object identity, event transactions
and detachable fragments are in place. Phase 1 tasks 1–7 and task 10 are
implemented. Tasks 8, 9 and 11 are substantially implemented but still have
the remaining work listed below. Clipboard and paste semantics are Phase 2.

The ARA integration has not been verified against a real ARA plug-in.

| Phase 1 area | Status |
| --- | --- |
| History core and operation API | Implemented |
| Clip properties, structure and compounds | Implemented |
| Gesture scopes and compatible coalescing | Implemented in the command/history layer; additional callers may still need integration |
| Track add, remove and restore | Implemented, including asynchronous preparation and failure cleanup |
| Plug-in graph, parameter, state and lifecycle operations | Implemented for command surfaces; unprovenanceable plug-in notifications are synchronization-only |
| Native, JavaScript and MCP history control | Implemented; persistent progress UI remains |
| Save point and dirty state | Implemented in the history layer; exposed with pending plug-in state by `SequencerEngine` |
| Resource retention and replay failures | Core policy implemented and tested; relinking UX remains |

## Why undo precedes the clipboard

Copy and paste are document mutations. Once the mutation funnel and detached
fragments exist, paste and duplicate can reuse them and become undoable without
a second insertion architecture. Undoing a deletion also needs the same
self-contained fragment that a clipboard payload needs, so the dependency runs
from undo to clipboard rather than the other way around.

## Module responsibilities

`uapmd-data` provides `ProjectCommand`, `ProjectCommandManager`, `ProjectHistory`
and the operation-level `ProjectUndoEngine`, along with persistent addresses,
fragment types and document-event transactions. These contracts do not require
`uapmd-engine`.

`uapmd-engine` provides `ProjectCommands`, concrete commands and mutation
primitives in `TimelineFacade`, including resource preparation and fragment
capture/restore. `TimelineFacadeImpl` owns the history and command manager,
supplying model-thread dispatch and document-transaction callbacks. A
`ProjectHistoryFactory` passed at engine construction selects the history
implementation.

The default `CommandInverseHistory` adapts forward/revert command pairs onto
`ProjectUndoEngine`. `ProjectHistory` is expressed in commands so alternative
strategies, such as snapshots or journals, need not implement retained inverse
operations. `NullProjectHistory` executes commands without recording history;
it reports no history-derived dirty state and is unsuitable for callers that
use that state to decide whether to prompt for saving.

The sequencer, plug-in, ARA and application sections below describe integration
with this infrastructure, rather than additional responsibilities of
`uapmd-data`.

## Design contract

The existing document model remains the observer-facing API. A mutation layer
is the only path that changes document state and it emits the existing document
events as a projection of its committed activity.

Undo entries are created at the mutation boundary, not by listening to events.
Events contain post-change state and cannot reliably recover before-state or
gesture boundaries. A failed mutation must therefore fail at its call site,
not silently leave an incorrect history entry.

The default history uses typed commands and their inverses rather than
whole-project snapshots or JSON Patch. This is the default implementation's
strategy, not a restriction on `ProjectHistory` implementations. Property
commands retain the values needed in each direction. Structural commands retain
detached fragments and opaque extension data. Every operation addresses tracks,
clips and graph nodes by persistent identity, never by a mutable vector index.

The authoritative project model remains mutable. Immutable real-time snapshots
are a separate audio-publication mechanism; undo does not replace them with a
copy-on-write project tree.

## Asynchronous execution

Commands expose callback-based `execute` completion, and `ProjectHistory`
exposes callback-based undo and redo. The default history uses
`ProjectUndoEngine`, whose operations expose asynchronous `perform`, `undo` and
`redo` completion; there is no separate synchronous variant of that engine.
Simple property commands complete inline, while plug-in and track commands may
prepare resources asynchronously.

History state and document commits are serialized on the model thread. Callbacks
from plug-in or file APIs must be dispatched back to that thread before they
touch the document or history. The audio thread never participates in history
execution.

The history cursor advances only after the requested operation succeeds. A
pending operation makes competing history mutations busy. A failed operation is
not added to history; a failed undo or redo leaves the cursor and the pending
entry unchanged.

Fallible preparation happens before the visible document commit where possible.
Compound operations execute children in reverse order for undo and forward order
for redo. If a later child fails after earlier children have committed, the
completed children are compensated. A half-applied compound step is not a valid
success or failure result.

Replay uses the same mutation primitives with history recording disabled.
Ordinary document events, graph updates and derived cache invalidation remain
enabled. Replay suppression is carried in an operation context rather than a
process-wide flag.

`ProjectDocumentTransaction` batches observer events. It is not an undo scope,
does not capture inverses and does not roll back failed work. Named compounds
and gesture scopes are explicit history scopes.

## Foundation

### Persistent identity

Tracks, clips and plug-in graph nodes have identifiers that survive save/load
and can be restored. Readers mint positional identifiers for older files that
do not contain IDs. Graph node IDs are persisted instead of being derived from
session-local plug-in instance IDs.

### Mutation funnel and transactions

`ProjectCommands` supplies concrete sequencer commands to
`ProjectCommandManager`, which centralizes mutation-origin policy, history
interaction, model-thread completion dispatch and event-transaction scoping.
For recorded edits, `ProjectHistory` executes the command so it can capture
state before mutation. `TimelineFacade` supplies the underlying mutation
primitives and emits document events from the same committed path. Event
transactions nest; only the outermost transaction flushes. Events outside an explicit transaction are delivered as a batch of
one. Repeated events for the same object and event kind may collapse, with
listeners re-reading the current document state at the batch boundary.

Transactions must not remain open across asynchronous external work, including
plug-in state requests and ARA archiving. Such commands batch only their
synchronous commits. Steps and gestures default to per-command event batching;
whole-step batching is reserved for steps whose commands all complete inline.

### Detachable fragments

`ProjectClipFragment` and `ProjectTrackFragment` are non-destructive detached
representations used by undo and, later, the clipboard. Restore reuses the
captured identity; paste and duplicate will use `Mint` in Phase 2.

Track fragments include graph topology, plug-in state, clips and extension
state. Capturing or attaching a track may be asynchronous. Capture fails rather
than returning a fragment that silently lost extension or graph state.

### ARA invalidation

ARA content-change notifications revoke affected frozen renders. Notifications
that only indicate dirty state for re-archiving are ignored because projects
are written as complete archives. ARA region head/tail timing is not currently
queried; see the ARA follow-up section.

## Implemented Phase 1 operations

### Clip properties and content

Clip enablement, duration, name, file path, save state, markers, warps, gain
and mute are undoable property operations. Audio and MIDI content replacement
retains complete before/after clip fragments, including source-node content,
duration and extension state. Piano-roll/raw UMP edits and MIDI recording use
the same content operation.

Clip creation, deletion, clearing and restoration use `ProjectClipFragment`.
Deletion captures before the document edit so ARA archiving remains legal;
restore reuses the original persistent identity. MIDI imports that affect
musical and master-track clips group their changes into one compound step.

### Track structure and properties

Track creation, deletion and restoration use asynchronous fragment operations.
Preparation constructs plug-ins, state, clips and graph topology on a detached
track. The track is published only after preparation succeeds. Failure removes
the detached resources and does not advance history or expose a partial track.

Track and master-track gain, mute, solo, bypass and freeze policy are undoable.
Record-arm, input monitoring, playback compensation and latency settings use a
single snapshot operation. Device-input creation, routing and removal use typed
operations and avoid collisions with clip/source-node IDs.

### Plug-ins and graphs

Global and per-note parameter commands use persistent plug-in identity. Generic
hosted plug-in parameter notifications only synchronize the engine's cached
value; they do not create history. The formats do not identify whether such a
notification came from a human editor gesture, preset loading, state restore,
initialization or automation, so inferring history from it can corrupt undo and
redo. Hosted plug-in state-dirty notifications have the same limitation and
only refresh the cached opaque state.

Plug-in insertion and deletion restore format, identifier, persistent graph
node identity, UMP group and state. Insertion on a new track records the track,
instance, state, group and graph together. Graph type replacement, connection
edits, preset loading and whole-state replacement have undo/redo operations.

### Application surfaces

The native application exposes history state and asynchronous undo/redo. Menu
items and shortcuts report descriptions, busy state and failures. JavaScript
and MCP expose pollable mutation jobs, history queries and named remote
compound scopes. A remote mutation is one history step unless its client owns
an explicit compound scope.

## Save points and dirty state

`SequencerEngine::isProjectDirty()` combines the selected history's dirty state
with pending asynchronous plug-in mutations. The default history compares the
current history-node identity with the node marked saved. This handles undo
followed by a new edit, where a numeric cursor alone would be ambiguous.

File saves capture the history-node ID when serialization starts and mark that
node saved only after the write succeeds. Document-provider saves defer the
mark until the provider confirms the final write. Per-track dirty state is
runtime cache/render invalidation state and is owned by `SequencerEngine`, not
`AppModel`.

## Resource and retention policy

The default history uses command retention estimates to account for its
operations. `ProjectUndoEngine` evicts complete oldest history entries when the configured budget is exceeded, retains a newest
operation even when it alone exceeds the budget, and treats a compound as one
atomic budget unit. Clearing or evicting entries releases their fragments and
opaque state on the model thread, never on the audio thread.

Missing file sources and unavailable plug-ins are replay failures. The history
entry remains available for retry after the resource is restored, and detached
track construction ensures that failed plug-in restoration leaves no published
track or hosted instance behind.

## Remaining Phase 1 work

### Plug-in and graph adoption

- Route every authored parameter or state mutation through an explicit project
  command so its cause is known before it reaches the plug-in.
- Define a hosting contract with genuine human-gesture provenance before
  making native plug-in-editor changes undoable. VST3 begin/perform/end edit is
  insufficient: a plug-in may emit the same sequence while loading a preset.
  Playback automation remains excluded; authored automation data remains a
  document operation.
- Keep runtime metadata such as virtual MIDI labels outside project undo.

### Application and remote clients

- Add persistent progress and cancellation presentation for long-running native
  operations.
- Narrow replay notifications from full track-layout republishing to precise
  app-model deltas after all mutations are behind the funnel.
- Ensure future JavaScript/MCP device-routing surfaces use callback jobs or
  request identifiers and never block the model thread.
- Audit every persisted GUI, JavaScript and MCP edit and classify any remaining
  bypass as either an undoable document mutation or explicit runtime state.

### Resource UX

- Provide user-facing recovery or relinking for file-backed audio when a redo
  source is unavailable.

## ARA follow-up

The ARA content reader is available but has no consumer. A future consumer may
generate MIDI clips from detected notes, converting ARA seconds to ticks; the
resulting clip creation must use the mutation funnel.

ARA playback-region head and tail timing is not queried. General plug-in tail
handling is conservative but does not model per-region head time. Bounded
offline render and track-freeze paths will need a contributor hook for region
head requirements before this can be implemented and verified with a real ARA
plug-in.

Frozen render revocation from clip and document changes is implemented and
idempotent. Graph-change invalidation needs separate review because the freeze
renderer temporarily changes graphs while rendering.

## Verification

Coverage includes project-file backward compatibility, event batching, fragment
round trips, asynchronous plug-in creation/state restoration, compound and
gesture behavior, clip and track undo/redo, graph and parameter operations,
resource failures, memory-budget eviction, save-point dirty state and the
JavaScript/MCP history jobs. Plug-in lifecycle tests also send synchronization
notifications after several event-loop turns and from a worker thread, and
verify that they do not replace the add/remove history entries.

Tests do not replace verification with a real ARA plug-in or every platform
plug-in implementation. The current test suite uses deterministic plug-in and
audio-file doubles for those paths.

## Related documents

- [Sequencer](../uapmd-engine/SEQUENCER.md)
- [Track freezing](../uapmd-engine/TRACK_FREEZING.md)
- [API policy](../design/API_POLICY.md)
