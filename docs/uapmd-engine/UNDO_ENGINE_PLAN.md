# Undo Engine Plan (AI slop)

## Status

Planning only. No implementation has started.

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

### Mutation is not funnelled through the document

Callers reach into `ClipManager` directly and then separately call
`TimelineFacade::notifyClipChanged`. The method name is accurate: the event is
a courtesy call the caller must remember to make, not a consequence of the
mutation. `TimelineEditor::updateClipName` is representative — it calls
`clipManager().setClipName()` and then notifies as a distinct step.

### The project file stores no object identity

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

### 0.2 Funnel every mutation through the document

Add a document mutator covering the operations callers actually perform, and
migrate call sites off direct `ClipManager` access. Event emission moves
inside the mutator, so `notifyClipChanged` disappears as a public concept.

The mutating call sites outside the engine are bounded:

| Location | Operations | Sites |
| --- | --- | --- |
| `uapmd-app/gui/TimelineEditor.cpp` | `resizeClip` x5, `setClipAnchor` x2, `setClipName`, `setClipMarkers`, `setClipFilepath`, `setAudioWarps`, `clearAll` | 12 |
| `uapmd-app-model/src/AppModel.cpp` | `resizeClip` x2, `setClipMarkers`, `setClipEnabled`, `setClipAnchor`, `setAudioWarps`, `setClipNeedsFileSave`, `clearAll` | 8 |
| `uapmd-app/gui/TimelineEditor.cpp` | `replaceClipSourceNode` via `applyPianoRollEdits` | 1 |

Read-only access (`getClip`, `getAllClips`) is the larger share of the call
sites and does not move.

`TimelineEditor.cpp` is roughly 3300 lines with mutation logic interleaved
into ImGui drawing code. The migration is therefore done one operation family
at a time — resize, then anchor and position, then metadata, then content
replacement — with a build after each, rather than file by file.

### 0.3 Transactions

One user gesture becomes one unit.

- `beginTransaction` and `commit`, with nesting depth tracked. Rollback
  arrives with the undo stack in Phase 1 and is not part of Phase 0.
- Events are emitted on commit, coalesced per object, rather than one event
  per mutation.
- Composite operations become atomic. `applyPianoRollEdits` currently performs
  `replaceClipSourceNode`, then `resizeClip`, then the notification as three
  separate steps, so observers can observe the intermediate state.
- In the ARA addin, `beginEditing` and `endEditing` move from per-event to
  per-transaction scope. They are currently unguarded direct calls, so this
  also requires a depth guard to keep them balanced under nesting.

### 0.4 Detachable fragments

A lossless serializable representation of a clip or a track, detached from the
document. The same fragment is the undo payload for a delete and the clipboard
payload for a copy.

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

The project file format change in 0.1 is the highest-risk item and the only
step that can damage existing user projects. It needs a backward-compatible
read path and a round-trip check against a project saved by the current build
before anything else is layered on top of it.

The migration in 0.2 is the largest by volume, for the reasons given above.

The ARA addin falls back to a full `populateModel` resync for any event it
does not recognize. Intermediate states during this migration therefore remain
correct and merely become slower. An unexpected drop in ARA responsiveness is
the signal that a fast path needs re-tuning, not that the mirrored model has
diverged.

Each step ends with `cmake --build cmake-build-debug` passing before the next
one begins.

## Open questions

- Whether the persistent identifier should be a plain string or a UUID. This
  affects how readable and diff-friendly the project file remains.
- Whether the backward-compatible read path in 0.1 should mint identifiers and
  persist them on first load, or mint them per session until the next explicit
  save.

## Related documents

- `docs/uapmd-engine/SEQUENCER.md`
- `docs/uapmd-engine/TRACK_FREEZING.md`, which consumes the track reference
  identifiers discussed in 0.1
- `docs/design/API_POLICY.md`
