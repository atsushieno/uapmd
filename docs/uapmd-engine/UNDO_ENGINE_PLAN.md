# Undo Engine Plan (AI slop)

## Status

Phase 0 is done for its purpose: the mutation funnel, transactions, persistent
identity and detachable fragments all exist. Phase 1 is in progress through
structural clip operations and track-property history; the clipboard remains
Phase 2 and has not started.

Two pieces of ARA work are deliberately left for later and are described under
Remaining work. Neither gates Phase 1. The ARA work as a whole is unverified
against a real plug-in.

## Why undo comes before copy/paste

Copy/paste is expressible as document mutations. Given a layer where every
mutation is undoable, pasting is one more mutation and becomes undoable
without further work. Building the clipboard first produces a bespoke insertion
path that has to be retrofitted into undo later.

The dependency does not run the other way. A clipboard payload is a detached,
serializable document fragment, and undoing a delete needs exactly the same
fragment. Undo forces that machinery to exist; a clipboard contributes nothing
toward undo.

## Design contract

The existing document model stays as the public, observer-facing surface.
`ProjectDocumentView` and `ProjectDocumentEvents` keep their shape and remain
what ARA, the GUI, the scripting runtime and any future out-of-process observer
consume.

A mutation layer sits underneath. It is the only path that changes document
state, and it emits the existing document events as a projection of its own
activity when a transaction commits.

The undo entry is produced at the point of mutation, not by an event listener:

- The events carry object identity and post-change state only. A listener would
  need a shadow copy of the document to recover the before-state.
- Undo grouping is a property of the call site. One user gesture is one undo
  step, and that is not recoverable from a notification stream.
- A listener-based undo would silently record a wrong stack when a mutation
  fails to notify, rather than producing a visible bug.

## Phase 0 steps

**0.1 — Persistent identity.** Tracks and clips carry identifiers that survive
save and load and can be re-adopted on restore. Identity lives on
`UapmdClipDataReferencible`, the shared base of the project clip and track data
and the type the anchor system already addresses. The writer emits `id` only
when non-empty; the reader mints the positional identifier the pre-identity
writer would have used when it is absent, so older projects still load.

Object identity is core project structure and stays in the project document.
Clip anchors are not, and are to move into a separate file in the project
archive via `ProjectSerializationExtension`. Until then anchors keep their
positional form, which also keeps files this build writes readable by builds
without identity support.

**0.1c — Plugin graph node identity.** `AudioGraphNode::nodeId()` already
existed and is already the key everything downstream uses, but for plugin nodes
it held `"plugin:" + instanceId` — a session-local value. It is now persistent:
stored as `node_id` on the plugin node data, minted positionally when absent,
and passed back through `appendNodeSimple` and `addPluginToTrack` on load.
Required because ARA archives are keyed by it.

**0.2 — Mutation funnel.** `TimelineFacade` owns every clip mutation, each
applying the change and emitting its event as one step so an observer cannot
miss one. Callers no longer reach into `ClipManager` to mutate, except for clip
anchors, which are left alone because their storage is moving.

Rebuilding an audio clip's source needed the warp resolution that decides where
each warp point falls, and that resolution lived in the application — in three
copies, across `TimelineEditor`, `AppModel` and `ClipPreview`, one of them under
a different name. It could not move because it read the sample rate from an
`AppModel` singleton. Taking the sample rate as a parameter freed it, and it now
lives in `uapmd-data` as `TimeReferenceResolver`, next to the `TimeReference` it
resolves. All three copies are gone.

**0.3 — Transactions.** Batching lives in `ProjectDocumentEventDispatcher`, so
it applies to every emitter. Transactions nest and only the outermost flushes.
Every event reaches a listener inside a `transactionBegan`/`transactionEnded`
pair, including one emitted outside an explicit transaction, which is delivered
as a batch of one — that is what lets a listener move per-event work to the
batch boundary without knowing whether a transaction is open. Within a batch,
events matching on kind and object identity collapse; listeners re-read current
state, and a batch ships only after every mutation in it has been applied.

**0.4 — Detachable fragments.** A fragment is a detached copy of one document
object, and is the payload behind both undo and the clipboard. Capture is
non-destructive and removal is separate, so copy, cut and delete-with-undo
compose from the same operations. `ProjectObjectIdPolicy::Restore` reuses the
captured identifiers, which undoing a delete requires; `Mint` allocates fresh
ones for paste and duplicate.

A clip fragment is the values needed to recreate the clip. Building on
`UapmdProjectFile` was rejected: it exports MIDI content to a separate file, so
a fragment built on it would not be self-contained.

A track fragment additionally holds graph topology as bytes and per-plugin
state. Both capturing and attaching one are asynchronous, because reading
plugin state and creating plugin instances both are. `ProjectTrackAttachOptions`
carries a component mask — identifier policy, plugins, plugin state, clips —
because track duplication is not all-or-nothing in any DAW that offers it.
Capture always takes everything, so one fragment serves a clone, a partial
duplicate and an undo restore.

Extensions contribute their own opaque state to a fragment through
`ProjectSerializationExtension`, reusing the same `extensionId()` space that
keys `ProjectClipFragment::extensionState`. Capture fails outright rather than
returning a fragment that looks complete but has lost state.

**0.5 — ARA wiring.** The inbound notifications are handled.
`notifyPlaybackRegionContentChanged` and `notifyAudioModificationContentChanged`
revoke the affected track's frozen render, because an ARA edit is a user edit as
far as anything caching rendered audio is concerned. Invalidation is suppressed
only when `kARAContentUpdateSignalScopeRemainsUnchanged` is set, that flag being
the plug-in's guarantee that the rendered signal did not change; absent it, the
signal is assumed to have changed. An audio modification affects every playback
region intersecting it, and the implementation conservatively revokes every
track using that audio source.

`notifyRegionSequenceDataChanged` and `notifyDocumentDataChanged` are deliberate
no-ops. The reason is not that their state cannot affect us — private state can
influence a plug-in's behaviour and rendering — but that they are dirty-state
signals whose only purpose is letting a host skip re-archiving. Every save here
writes a fresh full archive, so ignoring them loses no persisted state, and any
audible or content change still arrives through the content-change
notifications.

Not implemented: playback region head and tail time. ARA requires the host to
re-query `getPlaybackRegionHeadAndTailTime` whenever a region's output signal
changes, and a plug-in may adjust head and tail on any model edit, including
edits that do not touch the region directly. uapmd never calls it and models no
per-region ARA head or tail, so region transitions that extend audio beyond a
region's bounds are not accounted for. The general plug-in tail handled by
`TailProcessManager` is a different, coarser concept and does not cover this.

Partial persistency covers clips and tracks. Archives name only the objects
covering one object, with `documentData` false, which is what the SDK documents
for archives imported elsewhere. Restore passes an archive-to-current
identifier mapping when the object came back under a different identity.

Archiving and restoring have opposite requirements: an archive may only be
created from a document that is not being edited, while a restore must happen
inside an edit cycle. Restoring during attach is therefore correct, and capture
refuses when called inside a transaction.

## Phase 1 — Undo and redo

Undo history belongs to `uapmd-engine`, at the mutation boundary shared by the
application, scripts and future remote clients. The application supplies
gesture boundaries and user-facing descriptions; the engine owns inverse
capture, execution, replay suppression and stack consistency.

There is one asynchronous undo engine from the outset. It has no separate
synchronous undo manager. A simple property edit completes its operation inline;
track and plug-in work completes later through the same callback-based
contract. This keeps synchronous edits cheap without baking a false promise
that `undo()` or `redo()` always finishes before returning.

History is a typed command/memento journal, not RFC 6902 and not a sequence of
whole-project snapshots. Operations address objects by persistent identifier,
never by a mutable array index or textual tree path. Property operations retain
before and after values; structural operations retain detached fragments and
opaque extension state. A JSON-patch representation may later be derived for
IPC, but it is not the engine's internal undo model.

The authoritative project model remains mutable. Existing immutable RT
snapshots remain a separate publication mechanism for the audio thread. Undo
does not replace them with one atomically swapped project tree: live plug-in
instances, readers and render state are execution resources derived from
document changes, not copyable document values.

### Execution contract

An undoable operation has asynchronous `perform`, `undo` and `redo` entry
points, each completing exactly once with success or an error. Synchronous
operations invoke that completion inline. History state and document commits
are serialized on the engine's model thread; callbacks from plug-in APIs must
be posted back there before touching either. The audio thread never participates
in history execution.

The history cursor moves only after the requested direction completes
successfully. While a history operation is pending, another undo, redo or user
mutation is rejected or queued by one explicit policy; it must not race the
pending operation. Audio playback may continue from the previously published
RT state.

Asynchronous operations separate preparation from document commit. Capturing
ARA/plugin state, opening assets and creating or configuring replacement
instances happen before the visible mutation where possible. A compound undo
step prepares all fallible resources first, then commits its actions in reverse
order for undo and forward order for redo inside one document event batch. If a
commit can still fail after changing the document, the operation must provide
compensation; reporting failure while leaving half of a step applied is not a
valid result.

Undo and redo replay through the same mutation primitives with history
recording disabled, but ordinary document events, graph updates and derived
cache invalidation remain enabled. Replay suppression is carried by an
operation context rather than a process-wide boolean, so it survives nested and
asynchronous work safely.

`ProjectDocumentTransaction` remains observer batching, not undo management.
It neither captures an inverse nor rolls back failed work. A named undo step is
a distinct scope. The two scopes usually cover the same final commit, but
capture and other asynchronous preparation necessarily happen before either
document editing or event delivery begins.

### Phase 1 tasks

1. **Asynchronous history core.** Add engine-owned undo and redo stacks,
   descriptions, branch truncation, a configurable memory budget, busy state
   and completion callbacks. Define the model-thread executor used to resume
   plug-in callbacks. Loading or closing a project cancels or drains pending
   work and clears history deterministically.

2. **Operation and context API.** Define typed operations plus execution
   context (`User`, `UndoRedo`, `Load`, `Remote`, `Internal`) and the
   prepare/commit/compensate contract. A new user edit after undo discards the
   redo branch. Failed operations do not enter history; failed undo/redo does
   not move the cursor.

3. **Clip property operations.** Move synchronous clip properties first. They
   capture before/after values at the mutation point, complete inline through
   the asynchronous API, and prove that replay emits the same document events
   without recursively recording history.

4. **Named compound steps.** Group multiple operations as one user action.
   Undo runs the children in reverse order and redo in forward order. Resource
   preparation and failure handling follow the execution contract above rather
   than relying on event batching for atomicity.

5. **Continuous gestures.** Capture the initial value when a gesture begins,
   apply intermediate values without appending steps, and commit one operation
   containing the initial and final values when it ends. Where no gesture
   boundary exists, coalesce only compatible operations matching type, object
   identity and property.

6. **Clip structure.** Implement add, delete and restore with
   `ProjectClipFragment`. Capture occurs before the document edit so ARA
   archiving remains legal. Undo restore reuses persistent identity. Paste and
   duplicate use `Mint` and remain Phase 2 clipboard work.

7. **Track structure.** Implement track add, delete and restore with
   `ProjectTrackFragment`. Capture and attachment already expose the
   asynchronous work that motivates the common execution contract. Missing
   plug-ins, failed state restoration and partial graph construction must be
   surfaced without advancing history or exposing a half-committed track.

8. **Plug-in graph and parameter operations.** Move graph edits behind the
   mutation layer, then record topology, instance lifecycle and state changes.
   Parameter playback automation never enters undo. User gestures originating
   in either the host UI or a hosted plug-in use their begin/end boundaries to
   produce one step; automation-data editing is a document operation of its
   own.

9. **Application surface.** Expose busy, can-undo/can-redo, descriptions and
   asynchronous undo/redo completion through the engine. Wire menu items,
   shortcuts, progress and errors in `uapmd-app`. A script or MCP mutation is
   one step unless it explicitly opens a named compound step.

10. **Save point and dirty state.** Mark the history state reached by a
    successful save. Compare history-node identity rather than only a numeric
    cursor, because undo followed by a new edit creates a different branch.
    Project load, resynchronization, cache maintenance and other internal work
    do not enter user history.

11. **Resource and retention policy.** Give every operation a history-size
    estimate and evict complete oldest steps under the memory budget. Define
    failure behaviour for missing files and unavailable plug-ins. Dropping a
    history entry releases its fragments and opaque archives off the audio
    thread.

### Implementation status

The asynchronous history foundation is implemented in `ProjectUndoEngine`.
It provides the operation and execution-context API, serialized completion on
the model thread, busy rejection, undo/redo branch management, state-identity
based dirty tracking, memory-budget eviction and deterministic shutdown. It is
owned and exposed by `TimelineFacade`; no synchronous undo manager exists.

Task 3 is implemented for clip enablement, duration, name, file path, file-save
state, markers and audio warps. These operations retain before/after values,
resolve the target by persistent track and clip identifiers, and replay through
the same event-emitting mutation primitive. Load and internal restoration use
an explicit mutation origin and do not enter user history.

Project load now rejects pending history work and replaces the old history with
a clean root only after loading succeeds. Project save records the history-node
identity captured when serialization began, so an edit made while asynchronous
plug-in state is being saved still leaves the current project dirty.

Task 4 is implemented as explicit named compound scopes. Successfully applied
children remain provisional until the scope ends, then enter history as one
step. Undo runs children in reverse and redo in forward order. Failed replay
compensates already completed children in the opposite direction; cancelling
an open scope uses the same asynchronous rollback. Nested scopes are rejected
for now.

Task 5's engine support is implemented as explicit gesture scopes. Within a
gesture, adjacent compatible operations merge only when their concrete value
type, persistent track identity, persistent clip identity and property key all
match. The retained operation keeps the first before-value and latest
after-value; returning to the initial value removes it. Intermediate mutations
still publish their ordinary document events. The present timeline drag UI
commits clip movement only on release, so it has no stream of intermediate
document mutations to wrap yet. Automatic time-window coalescing without a
gesture boundary remains deferred until a caller actually needs it.

Task 6 is implemented for ordinary clip creation, deletion, clearing and undo
restoration. Deletion captures a complete `ProjectClipFragment` before the
document edit, so extension and ARA archiving never occurs inside an edit
transaction. Undo restores the original persistent clip identity; redo removes
that identity again. Clearing a track captures every clip before mutation and
records the removals as one named compound step.

Creation paths capture the successfully constructed clip, including its minted
identity and extension-owned state, then use `recordPerformed()` to register
the inverse without invoking the forward mutation a second time. Failure to
capture or register history removes the new clip rather than leaving an
untracked user edit. MIDI files containing both musical and master-track clips,
and empty MIDI clip creation followed by its default resize, are compound undo
steps. Load and internal attachment bypass history explicitly.

Fragment restoration now reapplies the captured anchor, exact duration,
enablement, gain, mute, markers, warps and extension state. A failed extension
restore removes the partially attached clip and reports failure, leaving the
history cursor unchanged. Redo of file-backed audio still depends on reopening
the source file; a missing file is therefore a visible replay failure, as
required by the resource-failure policy. `Mint` attachment for paste and
duplicate remains Phase 2.

Task 7's engine layer is now implemented, but application adoption and fully
detached preparation remain open. `TimelineFacade` exposes asynchronous track
add and remove operations. A delete captures plugin state, clips, graph bytes
and extension state before removal, then records a persistent-ID operation;
undo recreates the track at its former index and redo resolves it by persistent
identity. Empty-track creation uses the same post-construction registration as
clip creation. Track history entries account for retained graph, plugin, clip
and extension data in the memory budget.

Track attachment now restores into a requested insertion position, rebuilds the
captured graph type and topology, and treats plugin creation, MIDI-group,
plugin-state, clip, graph and extension restoration failures as failures. It
removes the partially constructed track and reports the error, so the history
cursor does not advance. Track-fragment capture likewise fails if graph
serialization is unavailable instead of returning incomplete state.

Two parts keep task 7 from being called complete. First, plugin instances are
currently constructed only after a live engine track exists, so observers can
briefly see that provisional track during an asynchronous attach even though a
failure rolls it back. Fully satisfying the prepare-before-visible-commit rule
requires a detached track/plugin construction facility. Second,
`uapmd-app-model` adoption has started. Its primary GUI creation and deletion
paths now use callback-based `addTrack()` and `removeTrack()`, which update layout,
plugin registration/removal metadata, dirty state and user-visible errors only
from the engine operation's completion. Physical track removal now performs the
same hosted-plugin UI and virtual-MIDI cleanup as individual plugin removal.
GUI MIDI and split-audio imports create their tracks sequentially through that
API and group the resulting tracks and clips into one named history step.
Callback-based `removeAllTracks()` likewise removes from the end so mutable
indices cannot invalidate the remaining work and records the clear as one step.
The operation as a whole remains asynchronous: resource preparation may take
arbitrarily long, while only insertion of a fully prepared track into the live
document and publication of its events should be a short synchronous commit.

The synchronous `AppModel::addTrackLegacy()`, `removeTrackLegacy()`,
`removeAllTracksLegacy()` and `importMidiTracksFromFileLegacy()` remain
temporarily for JavaScript, WebAssembly and MCP callers. They still use the
tombstone/raw-engine policy and must be removed when those surfaces gain
callback jobs or promises, not wrapped around an asynchronous wait. Bootstrap
track creation is now explicitly internal and bypasses history without using a
public compatibility method. Plugin creation that implicitly creates a track
also remains on the legacy path until plugin insertion can join track creation
in one compound history step; recording the empty track alone would make redo
lose the subsequently added plugin.

Task 9's native application surface is partially implemented. `AppModel`
exposes history state and callback-based `undo()` and `redo()`. Successful
completion reconciles instance removal and restoration, republishes the whole
track layout, and invalidates frozen-track caches. The Command menu displays the
current undo and redo descriptions, disables unavailable or busy actions, and
supports Ctrl/Cmd-Z, Ctrl/Cmd-Shift-Z and Ctrl-Y. Failures reach the platform
error UI. Script and MCP history commands, explicit remote compound scopes and
a richer long-running progress surface remain open.

## Phase 1 remaining work

The history core is usable, but Phase 1 is not close to complete at the
application level. A document mutation is not considered adopted merely
because its enclosing clip or track can be deleted and restored: changing that
property directly must itself create a correctly described history step, and
every GUI, JavaScript and MCP entry point must use the same mutation primitive.

The following subsections record completed adoption steps and the mutations
that still bypass history.

### Step 1 completed: clip timing and authored content

The first implementation step is complete. Timeline dragging and explicit
time-reference changes now use stable-ID anchor operations, with derived anchor
resolution kept outside history. Audio and MIDI authored-content replacement
retain complete before/after clip fragments, including source-node content,
duration and extension state. Piano-roll/raw UMP edits and MIDI recording use
that content operation. Master markers, clip gain and clip mute also have
undoable mutation entry points. Multi-track MIDI import records the master-clip
anchor separately inside its compound step, so redo reproduces it.

### Step 2 completed: track properties and routing

Track and master-track gain, mute, solo, bypass and freeze-policy changes now
enter history through persistent track identity. Gain gestures merge into one
step, and the GUI's non-additive solo action uses one document transaction and
one named compound step for clearing other solos plus enabling the target.

Record-arm, input-monitoring, playback-compensation and latency settings use a
single snapshot operation, so changing several related settings is undoable as
one action. Device-input creation, channel routing and removal use a typed
operation that restores the source node and channels on replay. AppModel device
input allocation now avoids clip and other source-node IDs, while the track
rejects duplicate source IDs defensively.

The engine and native GUI paths for these properties use the same mutation
layer; derived freeze render state remains outside history. Regression coverage
now exercises property undo/redo, freeze policy and device routing, including
source-ID collision rejection.

### Track properties and routing follow-up

The remaining work is limited to adopting callback jobs or request identifiers
in any future JavaScript/MCP device-routing surface; those callers must not
block the model thread to simulate synchronous completion. Freeze state keeps
the policy in history while rendered caches, queued renders and progress remain
derived runtime state.

### Plug-ins and graphs

#### Step 3 first slice: parameter editing

Global parameter edits already use the persistent plug-in property operation.
Per-note controller edits now use the same operation, keyed by the persistent
track/node identity and controller context (group, channel or note). The host
GUI therefore records global and per-note edits alike, and its existing
parameter gesture callbacks coalesce a drag into one history item. Adapters
that only expose the legacy note-scoped API are supported through the new
read-back fallback; adapters without a readable value reject the edit rather
than creating an undo entry that cannot be replayed.

- Plug-in creation and deletion, including implicit track creation.
- Plug-in bypass.
- Plug-in parameters changed by hosted plug-in UI, JavaScript or MCP, including
  any parameter-notification path not yet routed through the facade.
- Loading plug-in state and unsolicited persistent state changes reported by a
  plug-in.
- UMP group assignment and persisted device/instance labels or configuration.
- Switching a track between the simple and full-DAG graph types.
- Adding and removing graph connections and other graph-topology replacement.

This is the outstanding substance of task 8. Plug-in insertion on a new track
must record the track, instance, state, group and graph placement as one
operation. Parameter changes require begin/end gesture integration; playback
automation remains excluded, while editing automation data is undoable authored
content. Loading a whole state requires capturing the previous opaque state
before applying the replacement. Graph operations must invalidate frozen-track
caches without allowing the freeze renderer's own temporary graph changes to
revoke themselves.

### Application, JavaScript and MCP adoption

- JavaScript/WebAssembly track add, remove, clear and synchronous multi-track
  MIDI import still use the `Legacy` APIs.
- MCP track creation still uses `addTrackLegacy()`.
- JavaScript parameter, bypass and graph calls now reach the facade; creation,
  deletion, state loading, and any remaining raw hosted-UI notification paths
  still need the callback-based history funnel.
- JavaScript and MCP cannot invoke undo or redo and cannot open, finish or
  cancel an explicit named compound scope.
- Long-running native operations expose only minimal busy feedback in the
  Command popup; persistent progress/cancellation presentation is still absent.

These surfaces need callback jobs, promises or request identifiers. They must
not simulate synchronous completion by blocking the model thread. A remote
mutation is one history step unless its client explicitly owns a named compound
scope, and abandoned scopes need deterministic cancellation.

### Known gaps in operations already presented as undoable

- Track attachment publishes a provisional live track before asynchronous
  plug-in construction and state restoration finish. Detached preparation is
  still required to prevent observers from seeing a half-built track.
- Real asynchronous plug-in track restoration has no integration coverage.
- File-backed audio redo intentionally fails when the source file is missing,
  but resource recovery or relinking UX is absent.
- App dirty state still combines history-node state with older local dirty
  flags. Undoing back to the saved history node therefore does not yet
  guarantee that the application reports a clean document.
- Replay currently republishes the complete track layout rather than emitting
  a precise app-model delta. This is functional but should be narrowed once
  every mutation is behind the funnel.

Phase 1 completion therefore requires an audit showing that every persisted
GUI, JavaScript and MCP edit either enters history or is explicitly classified
as internal/derived state. Project load, resynchronization, cache maintenance,
transport, scanning, rendering/export and UI-window state remain intentionally
outside undo.

## ARA remaining work

The first two items are ARA-only and deferred by choice, not blocked.

1. **A consumer for ARA content reading.** `AraSupport::readContent` is in
   place and nothing calls it. Notes are the candidate worth doing first:
   `AraContentNote` carries `frequency` as well as `pitchNumber`, so a MIDI 2.0
   sequencer can keep the detected pitch where a MIDI 1.0 host must round to a
   semitone. Generating a MIDI clip from detected notes is a document mutation
   and belongs behind the funnel; the work is mostly converting ARA's seconds
   to ticks, with note-off at `noteDuration` rather than `signalDuration`.

   Open: whether the generated clip lands on a new track or the source's own,
   and whether re-running replaces the previous result or adds beside it.

2. **ARA playback region head time.** Not started, and narrower than it first
   appears.

   Tail is already covered. ARA requires a plug-in's companion-API tail to be
   at least the maximum tail of its playback regions, and the offline renderer
   already extends its range by `tailLengthInSeconds`, so region tails are
   accounted for conservatively without querying them.

   Head is not. `renderLeadInSamples` is maximum plug-in *latency*, which is a
   different quantity, and nothing queries ARA head anywhere. It only matters
   where rendering is bounded — offline render and track freeze — because
   realtime playback runs the plug-in continuously and it receives the earlier
   audio regardless. The consequence is that a bounded render starting where a
   region needs earlier input can begin with incorrect output.

   The fix is to extend a bounded render's start by the maximum head across the
   track's regions and discard that pre-roll, re-querying on
   `notifyPlaybackRegionContentChanged`, which is already handled. The
   structural obstacle is the familiar one: the offline renderer is in the
   engine and ARA is an addin, so the value needs a contributor hook rather
   than a direct call. `TrackAudioProcessorExtension` will not serve — it is
   about performing processing, not about declaring a requirement.

   Deferred deliberately: this is audio-correctness code guarding a case that
   needs an ARA plug-in to reproduce, so writing it now would mean untestable
   code whose only job is correctness nobody can observe. It should wait for a
   plug-in to verify against.

3. **Frozen render revocation.** Done. `FrozenTrackManager` revokes a track's
   frozen render from the clip added, removed and changed document events,
   rather than depending on each call site to remember an
   `AppModel::markTrackDirty` call. Revocation is idempotent — it bumps a
   monotonic generation counter — so the existing calls remain harmless.

   Plugin graph changes deliberately do not trigger this, even though they also
   change what a track renders: freezing manipulates the track's graph, so
   revoking on graph changes risks a render that revokes itself. Wiring that up
   needs the freeze path examined first.

Deferred beyond Phase 0: the undo stack (Phase 1), the clipboard and paste
semantics including the track duplication options dialog (Phase 2), plugin
graph edits in the mutation layer, and parameter automation, whose
participation in undo at all is a Phase 1 decision.

## ARA content reading

We push content to the plug-in through `updateAudioSourceContent`. Reading it
back is permitted and intended: the SDK names acting as a detection engine, and
reading what a region actually plays. The constraint is narrower than
"don't import plug-in content" — content-reader output is not a substitute for
archiving the plug-in's opaque modification state, which stays the plug-in's.

The read level is not a free choice. Since ARA 2.0 a playback region's content
cannot be derived from its audio modification plus transformation flags,
because region transitions adjust notes at region borders. Content describing
what a region plays must be read at region level; source or modification level
is for content wanted untransformed, which is the detection case.

`requestAnalysis` already triggers analysis and completes when the plug-in
reports content changed, but `AraAnalysisResult` carries only which kinds
finished. `AraSupport::readContent` supplies the missing half, taking a plug-in
instance, a scope and a content kind and returning notes, tempo entries and bar
signatures. It is pull: the app learns something changed from the document and
analysis notifications, and reads when it wants the content.

Three constraints shape it:

- Reading is a call sequence — availability, grade, create reader, count, read
  events, destroy reader — with no other call to that document controller
  interleaved, so it is one unit inside the addin rather than exposed
  piecemeal.
- Event data belongs to the reader and is copied out before it is destroyed.
- Completion is not availability. A plug-in may reject or fail an analysis, so
  availability and grade are checked at read time rather than inferred from the
  completion callback.

Whether results reach our clips is a separate decision from being able to read
them. Nothing consumes the API yet, and the first consumer is where that gets
decided.

## Verification

The project file format change in 0.1 is the only work here that can damage
existing projects. Its backward-compatible read path is covered by
`source/tests/UapmdProjectFileTest.cpp`, whose pre-existing tests feed JSON
with no `id` member and so exercise the legacy path directly.

Event batching, fragment round trips, the component mask, and the failure
behaviour of capture are covered by tests. Not covered: anything requiring a
real ARA plug-in, and the asynchronous plugin chains in track capture and
attach, which need a plug-in to instantiate. Those remain unverified.

Two defects found along the way are recorded here. Anchors targeting
master-track clips never resolve, because the writer and reader disagree on the
identifier spelling and the master-track resolution loop is guarded by an
inverted condition. The remaining `AppModel::removeTrackLegacy` compatibility
path does not remove a track: it destroys the plugin instances, clears the clips
and tombstones the index in `hidden_tracks_`, so a track slot is never reclaimed.
The callback-based `AppModel::removeTrack` uses physical structural removal and
does not have that defect.

## Related documents

- `docs/uapmd-engine/SEQUENCER.md`
- `docs/uapmd-engine/TRACK_FREEZING.md`, which consumes the track reference
  identifiers discussed in 0.1
- `docs/design/API_POLICY.md`
