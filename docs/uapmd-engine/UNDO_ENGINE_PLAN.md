# Undo Engine Plan (AI slop)

## Status

Phase 0 is complete except for two carve-outs recorded below. Steps 0.1, 0.1c,
0.3, 0.4 and 0.5 are done; 0.5 is unverified against a real ARA plug-in. Step
0.2 is complete apart from the clip anchor operations, which wait on the anchor
sidecar described under 0.1, and the audio source node replacement. Each step
below records its own state.

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

Done.

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

As implemented, batching lives in `ProjectDocumentEventDispatcher` rather than
in the facade, so it applies to every emitter. `beginTransaction` and
`endTransaction` nest and only the outermost end flushes;
`ProjectDocumentTransaction` and `ScopedDocumentTransaction` are the scoped
forms, the latter being what callers outside the engine use.

Every event now reaches a listener inside a `transactionBegan` /
`transactionEnded` pair, including an event emitted outside any explicit
transaction, which is delivered as a batch of one. Making the bracket
unconditional is what lets a listener move its per-event work to the batch
boundary without needing to know whether a transaction is in progress.

Within a batch, events matching on kind and on track, clip and audio source
identity collapse to one. Kind is part of that identity, so an add is never
absorbed into a later change of the same object. This is safe because listeners
re-read current state from `ProjectDocumentView` when handling an event and a
batch is delivered only after every mutation in it has been applied. Revisions
are assigned at delivery rather than at emission, so they stay contiguous in
the order observers see them.

Both ARA listeners gained real behaviour from this. `AraSession` was doing a
full `resyncFromProjectDocument` per event and now defers to one per batch.
`AraHostDocumentController` routes all editing through a depth-guarded
`enterEditing` / `leaveEditing` pair, and defers `notifyModelUpdates` to the
close of the outermost cycle, so a batch of twenty clip changes produces one
edit cycle and one model-update notification instead of twenty of each.

Transactions were applied to the composite operations 0.2 exposed: the audio
warp rebuild in both `AppModel` and `TimelineEditor`, the MIDI and piano roll
edit paths, the clip file replacement, and `clearClipsFromTrack`, which is one
user action however many clips it removes.

The batching rules are covered by `ProjectDocumentEventDispatcherTest` in
`source/tests/UapmdProjectFileTest.cpp`: delivery outside a transaction,
nesting, collapsing, and revision contiguity.

### 0.4 Detachable fragments

Clip fragments are done. Track fragments and the ARA archive slot are not.

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

As implemented, `ProjectClipFragment` in `uapmd-data` is a `ClipData` plus the
authored MIDI content, plus a map of opaque per-feature state keyed by
extension identifier, which is where an ARA partial archive will go. Audio
clips carry no content because they are rebuilt from `clip.filepath`.

The first bullet above did not survive contact with the code. Building on
`UapmdProjectFile` was rejected because that format exports MIDI content to a
separate file rather than holding it, so a fragment built on it would not be
self-contained — which is the one property both undo and the clipboard need.
`SourceNode::saveState` and `loadState` looked like the alternative, but every
implementation of them is an empty stub. A fragment is therefore the set of
values needed to recreate the clip, which is close to what the existing
`addMidiClipToTrack` and `addAudioClipToTrack` already take.

Capture is deliberately non-destructive and removal stays a separate call, so
that copy, cut and delete-with-undo are all composed from the same two
operations inside one transaction rather than needing three entry points.

`ProjectObjectIdPolicy::Restore` reuses the captured identifiers, which is what
undoing a delete requires, and `Mint` allocates fresh ones for paste and
duplicate. Restore is implemented on the identity staging introduced in 0.1
rather than on a second mechanism.

`replaceMidiClipContent` landed here and took three of the six deferred
`replaceClipSourceNode` sites. One of them, the generic UMP modifier in
`AppModel`, emitted no document event at all before this. The remaining three
sites rebuild an `AudioFileSourceNode` from resolved warp points, which is a
different enough shape to want its own mutator.

Covered by `ClipFragmentRestoresUnderItsOriginalIdentity` and
`ClipFragmentMintsANewIdentityWhenAttachedAlongside` in
`source/tests/SequencerEngineOutputTest.cpp`, which exercise the round trip
through a real `SequencerEngine`.

Still open in this step: track fragments, and populating `extensionState` with
the ARA partial archive, which belongs with 0.5.

### 0.5 ARA wiring

Done, apart from verification against a real ARA plug-in.

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

The inbound notifications are implemented, but not as the dirty-state tracking
this plan originally described. That tracking was written and then removed,
because it had no consumer and should not have one.

Its two candidate uses both fail. Skipping the re-archiving of unchanged state
on save is unsafe here: `writeExtensionFile` writes into the project directory
being saved, with no carry-forward from a previous save, so a skipped archive
is simply absent and the state is lost on the next load. The SDK's optimisation
assumes incremental persistency that this host does not implement. Marking the
project as having unsaved changes is the other, and it is the right idea
expressed in the wrong place: an ARA edit is a user edit, so it should reach
the document and be marked dirty by the mutation funnel like any other change,
not through a side channel that would need its own special case in every
feature that cares.

What the notifications do instead is invalidate cached rendered audio, which is
the same reasoning applied where it does hold.
`notifyPlaybackRegionContentChanged` and
`notifyAudioModificationContentChanged` resolve the affected track and call
`FrozenTrackManager::projectTrackBecameDirty`, exactly as a user edit does. Both
are gated on `kARAContentUpdateSignalScopeRemainsUnchanged`: when the plug-in
states the rendered audio did not change there is nothing to revoke, and absent
that assurance the signal is assumed to have changed. Note that
`trackIndexForReferenceId` scans regular tracks only and returns -1 both for the
master track and for an unknown identifier, so the master track is resolved by
reference identifier first rather than inferred from that -1.

Partial persistency covers tracks as well as clips.
`storeArchiveStateForTrack` and `restoreArchiveStateForTrack` use the ARA 3.0
draft region sequence entries in the store and restore filters, a region
sequence being what a track maps onto, and `AraSupport` implements the
track-level fragment hooks with the same slot framing as the clip ones.

`notifyRegionSequenceDataChanged` and `notifyDocumentDataChanged` remain
deliberate no-ops. They report changes to state that is opaque to us and cannot
reach our clips, and since every ARA document is archived on every save, that
state cannot be lost by ignoring them.

The hook a fragment needs in order to carry such state now exists. Rather than
a parallel interface, `ProjectSerializationExtension` gained
`captureClipFragmentState` and `restoreClipFragmentState`, both defaulting to
no-ops. That reuses the existing registration and, more importantly, the same
`extensionId()` space that already keys `ProjectClipFragment::extensionState`.
It is unrelated to the addin mechanism in `uapmd-addin-core` despite the
similar word.

`captureClipFragment` asks every registered extension to contribute, and
`attachClipFragment` hands each its own slot back, addressed by the identity
the clip has at that moment: the captured one when restoring, a freshly minted
one when pasting. Extensions are invoked outside the registry lock, since one
may register or unregister another. Covered by
`ClipFragmentCarriesExtensionOwnedState`.

The ARA side of that hook is implemented.
`AraHostDocumentController::storeArchiveStateForClip` builds an
`ARAStoreObjectsFilter` naming only the audio source and audio modification
covering one clip, with `documentData` false, which is the configuration the
SDK documents for archives intended to be imported elsewhere.
`restoreArchiveStateForClip` is its counterpart and passes an
archive-to-current identifier mapping through `ARARestoreObjectsFilter` when
the clip came back under a different identity, as it does for a paste. A null
mapping means the archived identifiers are already the current ones, which is
the restore-in-place case.

`AraSupport` frames the fragment slot itself, because a clip's ARA state is not
one archive: every plug-in instance hosting an ARA document holds its own. The
slot carries one entry per document, keyed by the plug-in's persistent graph
node identity from 0.1c, each entry recording the persistent identifiers its
archive was taken from. An entry whose plug-in is not present when the fragment
is restored is skipped, which is what should happen when a fragment arrives
from another project or its plug-in has since been removed.

One SDK constraint shaped the design rather than merely being satisfied.
Archiving and restoring have opposite requirements: an archive "may only be
created from documents that are not being currently edited", while a restore is
documented as the sequence `beginEditing()`, `restoreObjectsFromArchive()`,
`endEditing()`. Restoring inside `attachClipFragment` is therefore correct,
since the surrounding transaction already holds an edit cycle open, but
capturing inside a transaction would be an illegal call. `captureClipFragment`
is documented as not being callable inside one, and the ARA side refuses rather
than making the call if it is. A cut is composed as capture first, then remove
inside a transaction.

Two defects in the pre-existing archiving path had to be fixed for any of this
to work, for fragments or for ordinary project save and load.

`getDocumentArchiveID` returned a host-invented string. The SDK defines it as
"the document archive ID that the plug-in's factory provided when saving the
archive", which plug-ins use to select a decoder when they support more than
one archive format; `ARAFactory::documentArchiveID` was read nowhere in the
codebase. The identifier is now taken from the factory, stored beside the
archive in both the project manifest and the fragment slot, and handed back on
restore. Relatedly, the SDK requires the host to resolve versioning conflicts
before restoring, so an archive is now refused unless the controller declares
its identifier as its own format or lists it among
`compatibleDocumentArchiveIDs`.

The other defect was that failure was indistinguishable from success. A clip
with no ARA objects and a clip whose state could not be archived both produced
an empty archive and reported success, so a fragment could silently lose
plug-in state and only reveal it when a user undid a delete and found their
edits gone. Capture now separates the two: no ARA objects still succeeds
trivially, while a clip that has state which cannot be captured fails, and
`captureClipFragment` abandons the whole capture rather than returning a
fragment that merely looks complete. It also refuses outright when called
inside a transaction, reporting the mistake at the call site instead of leaving
a slot mysteriously absent.

The failure behaviour is covered by
`ClipFragmentCaptureFailsRatherThanLosingExtensionState` and
`ClipFragmentCaptureIsRefusedInsideATransaction`. The success path is not, and
cannot be without a real ARA plug-in: it needs one loaded, a clip copied, and
the state observed to survive. That verification is deliberately deferred.

## Remaining work

Collected from the per-step notes above so that what is left is visible in one
place. Ordered by dependency, not by priority.

1. **Persistent identity for plugin graph nodes (0.1c).** Done.

   `AudioGraphNode::nodeId()` already existed and is already the key everything
   downstream uses — `getNode()` lookups, the DAG's indegree and topological
   order maps, connection endpoints. For plugin nodes it merely held
   `"plugin:" + instanceId`, a string wrapper around a session-local integer.
   The fix was therefore to change how that string is chosen rather than to
   introduce a second identifier: `AudioPluginNodeImpl` now accepts a node id,
   `appendNodeSimple` and `SequencerEngine::addPluginToTrack` take an optional
   restore id, and the project loader passes the stored one. An empty id still
   derives the old value, so newly added plugins behave as before.

   `UapmdProjectPluginNodeData` gained a `node_id` member, written from the live
   node's identity on save and minted positionally when absent on load. It is
   deliberately not called `id`: `plugin_id` already means the plugin *type*.

   Back compatibility came out free for DAG projects. They already carried
   `"plugin:<n>"` strings in both node descriptors and connection endpoints, so
   on load those simply become the persistent identities and the connections
   resolve against themselves. No migration is needed.

   The ARA archive manifest moved from keying entries by runtime instance id to
   keying them by node id, and its version went to `uapmd-ara-state-v2`. v1
   manifests are still read, but their entries can only be applied positionally
   and may bind to the wrong plugin — nothing better is recoverable from an
   identifier that no longer means anything. Archive files keep positional
   names so that node ids never have to be escaped into filenames.

2. **ARA wiring (0.5).** Done, unverified against a real plug-in.

3. **Audio source node replacement.** The three remaining
   `replaceClipSourceNode` call sites rebuild an `AudioFileSourceNode` from
   resolved warp points. They need their own mutator, in the shape of
   `replaceMidiClipContent`.

4. **Track fragments.** Done.

   A track fragment can hold its engine state by value, the same way a clip
   fragment holds MIDI content. Both halves are already produced as byte
   vectors: graph topology through `AudioGraphProvider::saveProjectGraph`, and
   per-plugin state through `AudioPluginInstance::requestState`. The external
   `.graph.json` and plugin state blob files are artifacts of saving a project,
   not of the serialization itself, so a fragment does not need them.

   What does make a track unlike a clip is that **both** capture and attach are
   asynchronous. `requestState` is callback-based, so capture cannot have the
   `std::optional<ProjectClipFragment>` shape that `captureClipFragment` has;
   and plugin instantiation through `addPluginToTrack` is callback-based, so
   attach cannot return a finished track. The consequence for undo is the
   sharper one: deleting a track has to capture plugin state before destroying
   the instances, so the delete itself does not complete synchronously and its
   undo entry is an asynchronous command rather than a captured value.

   `saveProject` already sequences per-plugin `requestState` calls through a
   `runNext` chain with a pending counter. That is the pattern to follow rather
   than inventing a second one.

   Duplication is also not all-or-nothing. Every DAW that offers track
   duplication offers a choice of what comes along — plugins with or without
   clips being the common split — so attach takes a component mask. Capture
   stays uniform and captures everything, so that one fragment serves a clone,
   a partial duplicate and an undo restore.

   As implemented, `captureTrackFragment` and `attachTrackFragment` are both
   callback-based for the reasons above, and `ProjectTrackAttachOptions`
   carries the mask: identifier policy, plugins, plugin state, clips. Plugin
   state is a separate flag from the plugins themselves because instantiating
   without restoring state is both the most common duplication variant and the
   one that skips the slowest work. Covered by
   `TrackFragmentRoundTripsContentAndIdentity`,
   `TrackFragmentAttachHonoursTheComponentMask` and
   `TrackFragmentCaptureIsRefusedInsideATransaction`.

5. **Clip anchors.** Moving anchor storage out of the project document into an
   extension sidecar, then migrating the three deferred `setClipAnchor` call
   sites onto a facade mutator.

6. **ARA content write-back.** Changes a plug-in makes to its content do not
   reach our clips at all. `updateAudioSourceContent` runs host to plug-in
   only; nothing reads an ARA content reader back into a clip, so
   `notifyAudioModificationContentChanged` updates nothing in the document.
   This is the gap that made per-feature dirty tracking look necessary in the
   first place. Routing plug-in content changes back through the mutation
   funnel would give dirtiness, document events, frozen render revocation and
   undo from the existing machinery rather than one special case each. It is a
   feature in its own right and wants its own design pass.

7. **Frozen render revocation.** Move it inside the mutation funnel.
   `FrozenTrackManager` already listens for document events but its
   `clipAdded`, `clipRemoved` and `clipChanged` handlers are empty stubs, so the
   only thing revoking a stale frozen render is the hand-written
   `AppModel::markTrackDirty` call at each site. Implementing those handlers is
   the clean fix; the work is auditing the existing sites for double revocation.

Two defects found along the way are recorded here rather than fixed, because
neither belongs to this plan. Anchors that target master-track clips never
resolve, because the writer and reader disagree on the identifier spelling and
the master-track resolution loop is guarded by an inverted condition.
Separately, `AppModel::removeTrack` does not remove a track — it destroys the
plugin instances, clears the clips and tombstones the index in
`hidden_tracks_`, so a track slot is never reclaimed.

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
