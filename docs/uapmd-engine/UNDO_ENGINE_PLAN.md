# Undo Engine Plan (AI slop)

## Status

Phase 0 is done for its purpose: the mutation funnel, transactions, persistent
identity and detachable fragments all exist, so Phase 1 is unblocked. The undo
stack itself is Phase 1 and the clipboard is Phase 2; neither has started.

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
plug-in state is being saved still leaves the current project dirty. The next
implementation slice is named compound steps, followed by continuous gestures
and structural clip operations.

## Remaining work

Both items are ARA-only and deferred by choice, not blocked.

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

Two defects found along the way are recorded but not fixed. Anchors targeting
master-track clips never resolve, because the writer and reader disagree on the
identifier spelling and the master-track resolution loop is guarded by an
inverted condition. And `AppModel::removeTrack` does not remove a track — it
destroys the plugin instances, clears the clips and tombstones the index in
`hidden_tracks_`, so a track slot is never reclaimed.

## Related documents

- `docs/uapmd-engine/SEQUENCER.md`
- `docs/uapmd-engine/TRACK_FREEZING.md`, which consumes the track reference
  identifiers discussed in 0.1
- `docs/design/API_POLICY.md`
