# Undo Engine Plan (AI slop)

## Status

Phase 0 in progress. Step 0.1 is complete; step 0.2 is complete apart from the
clip anchor operations, which wait on the anchor sidecar described under 0.1.
Steps 0.3 to 0.5 have not been started. Each step below records its own state.

This document describes Phase 0, the shared groundwork that both undo/redo
and copy/paste require. The undo stack itself (Phase 1) and the clipboard
(Phase 2) are deferred and are described here only far enough to explain why
Phase 0 is shaped the way it is.

The plan was derived from the working tree at `main` and from the ARA SDK
revision currently pinned in `source/CMakeLists.txt`, which is the `develop`
branch at `25e830b` (`releases/2.3.0-67`).

## Why undo comes before copy/paste

Copy/paste is expressible as document mutations. Given a layer where every
mutation is undoable, pasting is one more mutation and becomes undoable
without further work. Building the clipboard first instead produces a bespoke
insertion path that has to be retrofitted into undo later.

The dependency does not run the other way. A clipboard payload is a detached,
serializable document fragment, and undoing a delete needs exactly the same
detached fragment. Undo forces that machinery to exist; a clipboard on its own
contributes nothing toward undo.

## Design contract

Two layers, deliberately separate.

The existing document model stays as the public, observer-facing surface.
`ProjectDocumentView` and `ProjectDocumentEvents` keep their present shape and
remain what ARA, the GUI, the scripting runtime and any future out-of-process
observer consume.

A new internal mutation layer sits underneath. It becomes the only path that
changes document state, and it emits the existing document events as a
projection of its own activity on transaction commit.

The undo entry is produced at the point of mutation, not by an event listener.
Three reasons:

- The existing events carry object identity and post-change state only. A
  listener would have to maintain a shadow copy of the document to recover the
  before-state, which is a second model to keep in sync.
- Undo grouping is a property of the call site. One user gesture is one undo
  step, and that is not recoverable from a notification stream where a
  thirty-note piano roll edit and a project load are both single events.
- Mutation is not currently guaranteed to notify (see below). A listener-based
  undo would silently record a wrong stack rather than produce a visible bug.

The mutation layer therefore holds before-state and transaction grouping, and
observers never see either.

## Findings that drive the plan

These describe the state of the code when the plan was written. The first two
have since been addressed by steps 0.1 and 0.2 and are kept because they are
the reasoning behind those steps.

### Mutation is not funnelled through the document

Addressed by 0.2 for every clip operation except anchors.

Callers reach into `ClipManager` directly and then separately call
`TimelineFacade::notifyClipChanged`. The method name is accurate: the event is
a courtesy call the caller must remember to make, not a consequence of the
mutation. `TimelineEditor::updateClipName` is representative — it calls
`clipManager().setClipName()` and then notifies as a distinct step.

The cost of this was not hypothetical. Both `clearAll` call sites removed every
clip on a track and emitted nothing at all, which 0.2 found and fixed.

### The project file stores no object identity

Addressed by 0.1.

`UapmdProjectClipData` and `UapmdProjectTrackData` have no identity field.
Objects are identified by their position in the serialized array. Runtime
identifiers are regenerated from insertion order on every load: track
reference IDs come from a monotonic counter in `onTrackAdded`, and
`ClipData::referenceId` is derived from the clip counter in
`ClipManager::addClip`.

Inserting or reordering a track therefore shifts every subsequent identifier.
This is already a defect independent of undo. `FrozenTrackManager` keys
frozen-render state by track reference ID, and the ARA addin derives its
persistent IDs from the same values. Both can re-bind to the wrong object
after a reorder and reload.

### Part of the document lives inside the plugin

ARA plug-ins hold opaque state per audio modification, per region sequence and
per document that the host cannot read or diff, only archive. The
corresponding inbound notifications on `ARAModelUpdateControllerInterface` —
`notifyAudioModificationContentChanged`, `notifyDocumentDataChanged` and the
ARA 3.0 draft `notifyRegionSequenceDataChanged` — are currently implemented as
empty stubs.

Undo of an action that touched an ARA-attached object cannot be expressed as
host-side before/after state alone. It needs an ARA archive captured before
the change and restored on revert.

## Steps

The steps are ordered by dependency. 0.1 precedes 0.4, and 0.2 precedes 0.3.
Each step is independently buildable and independently verifiable.

### 0.1 Persistent, restorable and remappable identity

Done.

Give tracks and clips identifiers that survive save and load and that can be
re-adopted on restore.

- Add an identity field to `UapmdProjectClipData` and `UapmdProjectTrackData`,
  written and read by the project file serializer. This is a project file
  format change and needs a backward-compatible read path that mints
  identifiers when the field is absent.
- Introduce an identifier allocator owned by the document. Every creation API
  takes an optional restore identifier: absent mints a fresh one, present
  adopts the supplied one.
- Stop deriving `ClipData::referenceId` from the clip counter; carry the
  allocated identifier instead.

The two policies required here are the ones ARA already specifies in
`ARARestoreObjectsFilter`. Undoing a delete restores under the original
identifiers, and pasting restores under freshly minted ones through the
archive-to-current identifier mapping.

Object identity is core project structure and stays in the project document.
Clip anchors are not, and are to be moved out of the document into a separate
file packaged in the project archive, written through the existing
`ProjectSerializationExtension` hooks. Until that move happens, anchors keep
their present positional form and are regenerated on every write, so this step
leaves anchor serialization untouched. The project document is otherwise to be
kept as lean as possible.

As implemented, identity lives on `UapmdClipDataReferencible`, the shared base
of `UapmdProjectClipData` and `UapmdProjectTrackData` and the type the anchor
system already addresses, so neither concrete implementation changed. The
writer emits `id` only when it is non-empty, and the reader mints the
positional identifier the pre-identity writer would have used when the member
is absent. Anchors continue to be written positionally, which keeps files
produced by this build readable by builds without identity support; a reader
that met a persistent identifier in an `anchor` member would fail to resolve
it and drop the clip.

`TimelineFacade` stages the persisted identifier for the next track or clip it
creates during a load, and creation consumes it instead of allocating.
`ClipManager::addClip` already preserved a pre-set `referenceId`, so the clip
side needed no change there. The track identifier allocator is advanced past
every restored identifier, otherwise a track added after a load would collide
with one the project already used.

### 0.2 Funnel every mutation through the document

Complete except for the clip anchor operations.

Add a document mutator covering the operations callers actually perform, and
migrate call sites off direct `ClipManager` access. Event emission moves
inside the mutator, so `notifyClipChanged` disappears as a public concept.

The mutating call sites outside the engine are bounded:

| Location | Operations | Sites | State |
| --- | --- | --- | --- |
| `uapmd-app/gui/TimelineEditor.cpp` | `resizeClip` x5, `setClipName`, `setClipMarkers`, `setClipFilepath`, `setAudioWarps`, `clearAll` | 10 | migrated |
| `uapmd-app-model/src/AppModel.cpp` | `resizeClip` x2, `setClipMarkers`, `setClipEnabled`, `setAudioWarps`, `setClipNeedsFileSave`, `clearAll` | 7 | migrated |
| both of the above | `setClipAnchor` | 3 | deferred |
| both of the above | `replaceClipSourceNode` | 6 | deferred to 0.4 |

Read-only access (`getClip`, `getAllClips`) is the larger share of the call
sites and does not move.

`TimelineEditor.cpp` is roughly 3300 lines with mutation logic interleaved
into ImGui drawing code. The migration is therefore done one operation family
at a time — resize, then metadata, then the rest — with a build after each,
rather than file by file.

`TimelineFacade` gained `resizeClip`, `setClipName`, `setClipFilepath`,
`setClipNeedsFileSave`, `setClipMarkers`, `setClipAudioWarps` and
`clearClipsFromTrack`. All of them, including the pre-existing
`setClipEnabled`, route through one private helper that applies the change,
emits the matching event and refreshes the timeline, so a mutator added later
cannot silently skip its event.

Two corrections to the survey this plan was written from. `replaceClipSourceNode`
has six call sites rather than one; it is declared on `TimelineTrack` rather
than `ClipManager` and so fell outside the original search. Those sites now sit
next to a facade call and do emit an event, but the source node swap itself
still bypasses the funnel, and it is left for the `replaceClipContent` mutator
in 0.4 because it needs the fragment representation. Separately, both `clearAll`
call sites emitted no document event at all, so every clip on a cleared track
disappeared without notice and ARA went on mirroring playback regions for clips
that no longer existed. `clearClipsFromTrack` removes clips one at a time
through the existing removal path so that each produces its own event.

The anchor operations are deferred for two reasons: they carry logic that is
not purely a mutation, namely cycle rejection and a full anchor re-resolve,
and anchors are the subject of the sidecar move described under 0.1. Migrating
them now would mean designing the facade's anchor API twice.

One coupling remains outside the funnel. Call sites still invoke
`AppModel::markTrackDirty` by hand, and that call does more than record an
unsaved-changes flag: it notifies `FrozenTrackManager` that the track changed,
so a mutation that omits it leaves a stale frozen render published. That is the
same failure mode as a missing document event and belongs inside the funnel,
but pulling it in requires auditing every site for double revocation.

### 0.3 Transactions

Not started.

One user gesture becomes one unit.

- `beginTransaction` and `commit`, with nesting depth tracked. Rollback
  arrives with the undo stack in Phase 1 and is not part of Phase 0.
- Events are emitted on commit, coalesced per object, rather than one event
  per mutation.
- Composite operations become atomic. `applyPianoRollEdits` currently performs
  `replaceClipSourceNode`, then `resizeClip`, then the notification as three
  separate steps, so observers can observe the intermediate state. Completing
  0.2 made this more visible rather than less: the audio warp rebuild in
  `AppModel` previously emitted nothing at all and now emits three events —
  markers, warps and duration — where one transaction is wanted.
- In the ARA addin, `beginEditing` and `endEditing` move from per-event to
  per-transaction scope. They are currently unguarded direct calls, so this
  also requires a depth guard to keep them balanced under nesting.

### 0.4 Detachable fragments

Not started.

A lossless serializable representation of a clip or a track, detached from the
document. The same fragment is the undo payload for a delete and the clipboard
payload for a copy. This step also owns the `replaceClipContent` mutator that
0.2 left behind, since replacing a clip's source node is the same operation as
attaching new content to an existing clip.

- Build it on the `UapmdProjectFile` serialization rather than introducing a
  second representation. Two independent notions of what a clip is will drift
  apart.
- A fragment is a pair: host-side data, plus an ARA partial archive covering
  the objects it contains.
- The operations are detach by identifier, and attach to a parent under an
  identifier policy of either restore or mint.

MIDI content is cheap to capture whole. The piano roll already rebuilds the
entire `MidiClipSourceNode` on every commit, so whole-clip capture is both
correct and the natural granularity. Per-note deltas are not needed.

### 0.5 ARA wiring

Not started.

Re-point the addin at the new emission source and close the opaque-state gap,
keeping all ARA structure handling inside `uapmd-ara`.

- Implement the three stubbed notifications so that per-object ARA state is
  marked dirty, ensuring a fragment captures a current archive rather than a
  stale one.
- Add the partial persistency path: `documentData` set to false with a subset
  filter for fragments, which is what the SDK documents as the intended
  configuration for copy/paste archives.
- Restore with the archive-to-current identifier mapping for the mint policy,
  and with a null mapping for the restore policy.

Region sequence persistency is ARA 3.0 draft API and can still change. ARA's
concepts — stable persistent identifiers, partial archives, identifier
remapping on restore — are allowed to shape the document model, but the
structure filling stays confined to the addin so that a draft revision does
not propagate into `uapmd-data`.

## Scope boundaries

In Phase 0:

- Persistent identity, including the project file format field.
- The mutation funnel for tracks and clips.
- Transaction grouping and coalesced event emission.
- Detach and attach fragments with an identifier policy.
- ARA dirty tracking and partial persistency.

Deferred:

- The undo stack and command objects (Phase 1).
- The clipboard and paste semantics (Phase 2).
- Moving clip anchors out of the project document into an extension sidecar.
- Plugin graph edits in the mutation layer.
- Parameter automation. It is a high-rate stream and whether it participates
  in undo at all is a Phase 1 decision.

## Risks and verification

The project file format change in 0.1 was the highest-risk item and the only
step that can damage existing user projects. Its backward-compatible read path
is covered by `source/tests/UapmdProjectFileTest.cpp`: the tests that predate
this work feed hand-written JSON with no `id` member and so exercise the legacy
path directly, and two tests were added for identifier minting and for the
requirement that a round trip leaves anchors positional.

The facade wiring added in 0.1 and 0.2 compiles and breaks no existing test,
but no automated test drives a save and reload through `SequencerEngine`, so
identity round-tripping at that level currently rests on inspection. The engine
test harness does expose `SequencerEngine::create`, so such a test is possible
if the coupling is judged worth pinning.

The migration in 0.2 was the largest by volume, for the reasons given above.

The ARA addin falls back to a full `populateModel` resync for any event it
does not recognize. Intermediate states during this migration therefore remain
correct and merely become slower. An unexpected drop in ARA responsiveness is
the signal that a fast path needs re-tuning, not that the mirrored model has
diverged.

Each step ends with `cmake --build cmake-build-debug` passing before the next
one begins.

## Settled during implementation

- The persistent identifier is a plain string, not a UUID. The project file is
  diff-friendly JSON and the identifiers already in use read as `track_0`.
- Identity stays in the project document rather than moving to a sidecar with
  the anchors. It is core project structure rather than a feature detail.
- The backward-compatible read path mints identifiers while reading and they
  are written out by the next save, rather than being regenerated per session.

## Open questions

- When the anchor sidecar lands, and what its extension identifier and file
  layout are. The three deferred `setClipAnchor` call sites and the facade's
  anchor API both wait on this.
- Whether frozen-render revocation moves inside the mutation funnel, and how
  double revocation is avoided at sites that also call `markTrackDirty` for
  the unsaved-changes flag.
- Whether the engine-level identity round trip is worth pinning with a test.

## Related documents

- `docs/uapmd-engine/SEQUENCER.md`
- `docs/uapmd-engine/TRACK_FREEZING.md`, which consumes the track reference
  identifiers discussed in 0.1
- `docs/design/API_POLICY.md`
