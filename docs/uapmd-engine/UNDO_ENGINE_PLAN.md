# Undo Engine Plan (AI slop)

## Status

Phase 0 is complete except for the carve-outs listed under Remaining work. It
is unverified against a real ARA plug-in.

Phase 0 is the groundwork both undo/redo and copy/paste need. The undo stack
itself is Phase 1 and the clipboard is Phase 2; neither has started.

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

**0.5 — ARA wiring.** The inbound notifications are implemented.
`notifyPlaybackRegionContentChanged` and `notifyAudioModificationContentChanged`
revoke the affected track's frozen render, because an ARA edit is a user edit as
far as anything caching rendered audio is concerned. Both are gated on
`kARAContentUpdateSignalScopeRemainsUnchanged`.
`notifyRegionSequenceDataChanged` and `notifyDocumentDataChanged` are
deliberate no-ops: they report state that cannot reach our clips, and every ARA
document is archived on every save, so ignoring them loses nothing.

Partial persistency covers clips and tracks. Archives name only the objects
covering one object, with `documentData` false, which is what the SDK documents
for archives imported elsewhere. Restore passes an archive-to-current
identifier mapping when the object came back under a different identity.

Archiving and restoring have opposite requirements: an archive may only be
created from a document that is not being edited, while a restore must happen
inside an edit cycle. Restoring during attach is therefore correct, and capture
refuses when called inside a transaction.

## Remaining work

1. **ARA content interchange.** The interchange is one-directional. We push
   content to the plug-in through `updateAudioSourceContent` and never read any
   back: no audio modification, playback region or audio source content reader
   is created anywhere. So a plug-in's edits reach nothing of ours.

   This needs a design pass rather than an implementation, because "write the
   content back into our clips" presupposes an answer. Three separable things
   hide under it, and they do not have the same answer:

   - Knowing that something changed, for the unsaved-changes flag. Small, and
     the content-changed notifications that now revoke frozen renders are
     already the signal.
   - Reading plug-in content to display it — detected notes, pitch, timing on
     our own timeline. This is what ARA content readers exist for, and it is
     read-only: the host shows the content, the plug-in still owns it.
   - Storing the plug-in's edits in our document. This partially inverts ARA's
     model, in which the plug-in owns the edit and the host owns the source
     material, and is likely wrong as a general rule.

   The whole ARA layer is AI-designed and should be assessed as such rather
   than extended on its current assumptions.

2. **Frozen render revocation.** Done. `FrozenTrackManager` revokes a track's
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
