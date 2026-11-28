# Project Commands and Undo History (AI slop)

The native application, JavaScript and MCP share one undo history for the
current project. History covers authored changes submitted through project
commands, including clips, tracks, plug-in graphs and explicit plug-in
parameter and state edits. Transport controls, the playback loop, selection,
clipboard contents, rendering caches, plug-in scanning and window state stay
outside history.

## Module responsibilities

`uapmd-data` defines the document contracts, persistent addresses, fragments,
document events and the command and history interfaces. `ProjectCommands` is
the typed edit API. `ProjectCommandManager` applies the mutation-origin policy
and exposes history controls through `ProjectCommands::history()`.

`uapmd-engine` implements `ProjectCommands` and the sequencer mutations behind
it. Structural and asynchronous edit entry points also live on
`TimelineFacade`. `TimelineFacadeImpl` owns the history and command manager and
provides model-thread dispatch and document-transaction callbacks.

The default `CommandInverseHistory` stores forward and revert commands using
the operation-level `ProjectUndoEngine`. A `ProjectHistoryFactory` supplied
when constructing the engine can select another implementation. The
`ProjectHistory` interface receives commands, so an implementation can capture
snapshots or journal edits. `NullProjectHistory` executes commands without
retaining undo entries and always reports a clean history. Save prompts that
depend on history-derived dirty state require a history implementation that
tracks it.

## Recording and replay

Use the document view to read project state and the command entry points to
submit edits. The command manager sends `User` and `Remote` mutations to
history. It applies `Load`, `Internal` and `UndoRedo` mutations without
creating new entries.

A command captures the state it will overwrite and registers a reversing
command with `ProjectCommandContext::recordRevert()`. A successful edit with
no revert creates no history entry. Some structural operations can capture a
complete forward/revert pair only after constructing the new object. These use
`ProjectCommandManager::recordExecuted()` after the change and its history
payload are ready.

Undo runs the revert command. Redo runs the forward command. Each successful
execution can refresh the command for the opposite direction. Property
commands retain the values needed in each direction. Structural commands can
retain detached fragments and extension data. The same mutation primitives
emit document events and invalidate derived state during replay. Observers
receive the committed changes and can read the resulting document state.

Tracks, clips and plug-in graph nodes use persistent identities so replay can
find an object even when its index or runtime plug-in instance ID changes.
The project format stores these identities. The reader supplies positional
fallback IDs for older track and clip records without IDs.

## Asynchronous commands and event delivery

Commands and history operations complete through callbacks. Simple property
edits can complete inline. Track or plug-in operations may need to prepare
resources asynchronously. Document commits and history changes run on the model
thread. Callbacks from external APIs must dispatch back to it before touching
the document or registering a revert. Audio processing consumes real-time
snapshots independently of the mutable project and its history.

A pending history operation makes competing history operations return
`Busy`. Failed edits create no history entry. Failed undo or redo clears the
busy state and leaves the history cursor and entry in place. A successful new
edit after undo discards the redo branch.

`ProjectDocumentTransaction` batches observer events until the outermost
transaction closes. Events emitted outside a transaction arrive as a batch of
one. Repeated events with the same kind and track, clip and audio-source IDs
can collapse into one notification. Listeners can then reread the current
state from `ProjectDocumentView`.

An event transaction only controls delivery. History scopes define undo steps
and cancellation runs inverse operations. Keep event transactions closed
during external work such as plug-in state requests and ARA archiving. An
asynchronous command can open a transaction around its synchronous commit.

## Compound steps and gestures

Open a named step when one action submits several commands. For synchronous
actions, `ScopedCommandStep` cancels an uncommitted step on scope exit.
Asynchronous callers can pair `beginStep()` with `endStep()` or
`cancelStep()` after the pending children complete. Only one step or gesture
can be open at a time. Both default to per-command event delivery. Whole-step
event batching is intended for actions whose commands all finish inline.

A gesture can merge adjacent compatible commands with the same command ID,
keeping the initial revert and the final forward value. Intermediate values
still reach the document, so controls and observers update during the gesture.
The mixer uses this for gain and balance drags, and the built-in plug-in
parameter controls also use gesture scopes.

Undo and cancellation run children in reverse order. Redo runs them in
forward order. If a child fails during replay, the engine tries to reverse the
children that already succeeded in that attempt. A failure in this rollback
is reported alongside the original error and can leave the document partially
changed, while the history cursor stays in place.

## Supported edits

### Clips and MIDI content

Undoable clip properties include position and anchoring, start trim and
duration, enabled state, name, file path, file-save flag, markers, audio warps,
gain and mute. Audio and MIDI content replacement retains before and after
clip fragments. Authored MIDI edits such as piano-roll note changes, raw UMP
edits and recorded MIDI use the clip content path. A MIDI import that also
changes master-track content groups the edits into one step.

Clip creation, deletion and clearing use `ProjectClipFragment`. Deletion
captures the fragment before the document edit so extensions can archive
their state. Restoration reuses the original persistent identity. The fragment
carries clip and source-node data, MIDI content where applicable, and extension
state. File-backed audio may still depend on the source file.

### Tracks and routing

Track edits include gain, output panners, mute, solo, bypass, freeze policy,
and the presentation properties for color, organizational path and sort
order. Track properties address the master track where applicable. Project
configuration commands cover master-track markers and the latency and
monitoring settings snapshot, including record-arm and input monitoring.
Device-input creation, rerouting and removal are undoable.

Track creation and removal retain the data needed to replay them.
`ProjectTrackFragment` captures track settings, clips, graph topology,
plug-in state and extension data. Track restoration can prepare plug-ins and
their state asynchronously on a detached track. The track is published after
preparation succeeds. On preparation failure the detached resources are
cleaned up.

### Plug-ins and graphs

The command paths for global parameters, per-note controllers, bypass, UMP
group, presets and whole-state replacement participate in history. Plug-in
insertion and removal retain the format, plug-in identifier, persistent graph
node identity, UMP group and state. Adding a plug-in on a new track records the
track and its initial plug-in together. Graph type changes and connection
edits are also undoable.

Hosted parameter and state-dirty notifications synchronize cached values.
Those notifications can come from preset loading, state restore,
initialization, automation or a plug-in editor, so they do not establish the
origin of a new authored edit. Native plug-in editor gestures need a hosting
contract that can distinguish a human gesture from those other causes.

### Clipboard

Timeline copy captures `ProjectClipFragment` values in the application's
internal clipboard with relative time and track offsets. Copy changes only the
clipboard. Cut and group deletion each record one named history step for the
clip removals.

Paste can use offsets from the destination track or resolve the original
tracks by persistent ID. The initial paste uses `ProjectObjectIdPolicy::Mint`
to give the new clips fresh identities. The paste is one history step, and
redo restores those pasted clips using the identities recorded by that step.
Piano-roll cut, paste and group deletion commit their note changes through the
MIDI clip content path.

## Application and remote controls

The native application's System menu shows the next undo and redo descriptions
and disables those actions while history is busy. Menu actions and keyboard
shortcuts use asynchronous undo and redo and report failures. After successful
replay, the app model reconciles plug-in instances and republishes the track
layout.

JavaScript exposes `uapmd.sequencer.getHistoryState()`, `undo()`, `redo()` and
`beginCompound()` / `endCompound()` / `cancelCompound()`. MCP exposes the
corresponding `get_history_state`, `undo`, `redo`, `begin_compound`,
`end_compound` and `cancel_compound` tools. History queries and opening a scope
return immediately. For undo, redo, ending or cancelling a remote scope, poll
the returned mutation job using `uapmd.mutations.getJob()` or MCP
`get_mutation_job`. A remote scope uses the shared project history and is
visible to other application surfaces.

## Save points and retention

The default history compares the current history-state ID with the saved ID.
An open compound or gesture also counts as dirty. A fresh edit after undo
gets a new state ID, even if it lands at the same depth as the saved state.
`SequencerEngine::isProjectDirty()` also checks for pending asynchronous
plug-in mutations. Per-track dirty flags belong to the engine and serve
runtime cache and render invalidation.

File saves capture the history-state ID at the start of serialization and
mark that ID saved after a successful write. Document-provider saves mark it
saved after the provider confirms the final write. A later history state stays
dirty. Loading a project establishes a fresh, saved history root.

The default history budget is 64 MiB of estimated retained data. When enforcing
the budget, `ProjectUndoEngine` evicts the oldest complete undo entries while
keeping at least one undo entry. A compound counts as one unit. Clearing
history or dropping entries releases their retained fragments and opaque
state on the model thread.

A missing file source or unavailable plug-in can cause replay to fail.
The entry stays available for retry once the resource is restored. File-backed
audio relinking still needs a user-facing recovery path.

## ARA and verification

ARA state participates through clip fragment extension data. Content-change
notifications revoke affected frozen renders. A notification that only marks
the ARA document dirty for re-archiving does not create a history entry. The
host exposes an ARA content reader, but project editing does not currently
consume its results. ARA playback-region head and tail timing remains outside
the current implementation. Real ARA plug-in verification remains open.

Repository tests exercise project-file compatibility, document event batching,
fragment identity and round trips, clip and track replay, graph and plug-in
changes, asynchronous restoration and failure cleanup, history retention,
save points and JavaScript history jobs. Plug-in lifecycle tests use a test
host to check that delayed and worker-thread notifications leave the
add/remove history intact.

## Related documents

- [UAPMD app editing and clipboard controls](../users/UAPMD_APP.md#clips)
- [UAPMD project file format](UAPMD_PROJECT_FILE.md)
- [Sequencer](../uapmd-engine/SEQUENCER.md)
- [Track freezing](../uapmd-engine/TRACK_FREEZING.md)
- [API policy](../design/API_POLICY.md)
