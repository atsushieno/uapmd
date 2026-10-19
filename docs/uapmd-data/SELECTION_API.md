# Document Selection API (AI slop)

## Status and authorship

This is an AI-authored design record without maintainer edits. It records the
design choices for selection and the document API. The implementation status
below describes the first implementation; the design sketches remain provisional.

Selection should be a document service over stable object identities, with
independent selection contexts and explicit coordinate systems for ranges.
Tracks, clips, notes, and automation points use the same service. Containment
affects object lookup and edit behavior; it does not require separate selection
APIs for timeline objects and clip contents.

Selection is transient document interaction state. Changing it does not change
the document's edit revision, dirty state, saved content, or undo history.

## First implementation

The following contracts and implementations now live in `uapmd-data`:

- `ProjectDocument`, reached through `SequencerEngine::document()` or
  `TimelineFacade::document()`, exposes inspection, events, selections, MIDI
  content, commands, command manager, and document transactions.
- `ProjectSelectionService` manages a default timeline context, additional
  optionally scoped contexts, active context, validation, and notifications.
  `snapshot()` returns an optional value so a removed context is distinguishable
  from an empty selection. `defaultContext()` and `hasContext()` support caller
  lifetime management.
- `ProjectSelectionRegion` supports the three coordinate/scope types described
  below. Time intervals are half-open, with equal endpoints representing a
  point. Pitch and normalized-value constraints include both endpoints. An
  absent timeline track list means all tracks; a present empty list means none.
- `MidiClipDocument` owns shared editable notes and automation, committed
  snapshots, stable object IDs, normalization, and encoding. Existing editor
  field names are retained for compatibility. `encodedSnapshot()` constructs a
  candidate without committing it. `ProjectCommands::setMidiClipContent()`
  applies it by persistent clip ID through the engine and records history.
- Data-only addresses, `ProjectCommands`, persisted latency settings, and
  `ProjectMasterTrackSnapshot` have moved into the data module. Compatibility
  facade aliases and headers remain in the engine module.

`TimelineFacadeImpl` implements the document interface using the existing engine
storage and the data-module services. It reconciles shared MIDI content and
selection against final document state before delivering a document batch, then
releases deferred selection notifications after document listeners finish.
Whole-clip replacement is batched across removal and restoration so undo retains
both selection and the shared MIDI model. Fragments retain committed MIDI
identities, restore them on undo, and mint new ones on paste. Recording append
uses an explicit encoding-offset mapping to retain existing logical identities;
extending a MIDI clip before its content start also carries its identities.

The application routes timeline and piano-roll selection through this service.
Automation points can be selected independently or together with notes. Editors
read API-driven selection changes, and render explicit timeline, note, and
controller-lane regions. UI-generated timeline ranges are currently published
when the range gesture completes; the service also supports continuous region
updates.

The native ARA adapter consumes the document API and forwards selection to
visible editor views, with an initial notification during UI setup/reopening.
Relevant document changes refresh its projection after model synchronization.
The AAP adapter uses the capability-negotiated `notify_selection` transport from
`aap-ara`. It maps the same projection to AAP playback-region and region-sequence
IDs plus an optional explicit project-time range, after model synchronization.
Plugins without `AAP_ARA_FEATURE_VIEW_SELECTION` report selection forwarding as
unsupported while retaining document support. AAP transports these notifications
over its Binder control path, outside audio processing.

The selection state and note/automation IDs are not added to project-file
serialization by this implementation. Identity-bearing fragments and history
retain them for the lifetime of the open document.

## Starting implementation and prerequisites

Before this work, selection used several separate representations:

- `AppModel` owned timeline clip selection in `selected_clip_references_`.
- `SequenceEditor` owned range-drag state and the requested interval used by the
  add-clip popup. There was no shared explicit range selection model.
- `PianoRollSession` owned editable notes, automation events, and note selection.
  Notes had session-local `edit_id` values. Its own commits preserved these IDs
  while updating raw event indexes, but `loadNotes()` assigned new IDs and cleared
  selection when content is reloaded, including after undo/redo.
- `PianoRollAutomationEvent` had a `rawEventIdx` but no equivalent stable edit
  identity.
- `ProjectDocumentView` and `ProjectDocumentEventSource` are in `uapmd-data`.
  Their concrete implementation, command services, and project lifetime are
  currently coordinated by `TimelineFacadeImpl` in `uapmd-engine`.
- ARA already binds the editor-view role and maps document clips and tracks to
  playback regions and region sequences. It did not yet forward host selection
  through `ARAEditorViewInterface::notifySelection()`.

Moving the existing selection containers into `uapmd-data` is insufficient.
Shared identities for notes and automation points must survive ordinary edits,
UMP reconstruction, and undo/redo.

Relevant current sources:

- [Document inspection API](../../source/uapmd-data/include/uapmd-data/detail/project/ProjectDocumentView.hpp)
- [Document events](../../source/uapmd-data/include/uapmd-data/detail/project/ProjectDocumentEvents.hpp)
- [Piano-roll data and session types](../../source/tools/uapmd-app-model/include/uapmd-app-model/detail/PianoRollTypes.hpp)
- [Piano-roll session implementation](../../source/tools/uapmd-app-model/src/PianoRollSession.cpp)
- [Timeline facade API](../../source/uapmd-engine/include/uapmd-engine/detail/sequencer/TimelineFacade.hpp)
- [ARA host document controller](../../source/uapmd-ara/src/AraHostDocumentController.cpp)

## Object identity

Reuse `ProjectObjectId`, with uniqueness within a document, for selectable
objects. Existing track and clip reference IDs provide a starting point; notes
and automation points need equivalent identities in the shared data model.

An ID identifies the object independently of its current parent, position,
duration, pitch, controller value, or encoded position. Object type and parentage
are available through document queries. The selection service stores object IDs
without needing separate containers for each object type.

In particular, selection identity must not be a track index, clip numeric ID,
event-array index, UMP word offset, or time/pitch/value tuple. A clip's current
track is location information, and moving it to another track must preserve its
selection identity. Existing compound addresses used by commands can be adapted
without making their current containment path the selection identity.

| Operation | Identity behavior |
| --- | --- |
| Move, resize, change pitch or value | Preserve identity |
| Reorder events or rebuild encoded UMP | Preserve identity |
| Move an existing object into another container | Preserve identity |
| Duplicate or paste a copy | Allocate new identities and remap internal relationships |
| Undo restoration | Restore original identities |
| Import unrelated replacement content | Allocate new identities unless explicit correspondence is supplied |

These guarantees cover the document's lifetime and its retained undo/redo
content. Persisting note and automation identities across save/reopen is a
separate decision. Transient selection does not require it.

## Common selection representation

```cpp
struct ProjectSelection {
    // Ordered by relevance; the first object is primary.
    std::vector<ProjectObjectId> objects;

    std::vector<ProjectSelectionRegion> regions;
};
```

Object IDs are deduplicated while preserving their order. A change in order can
change the primary selection and therefore counts as a selection change.

An editor can restrict the objects it accepts, but that is an interaction policy
of the editor. A combined editor can select different object types together
through the same representation and notification API.

Selecting a parent does not insert its children into `objects`. Commands decide
how dependent objects participate in an edit. For example, moving a selected note
may also move its per-note automation, even when those automation points are not
explicitly selected. Selecting an automation point directly is supported as
well. Both clip-level and per-note automation use stable object identities.

## Explicit regions and coordinate systems

Selected objects and explicit regions have different semantics. A marquee that
selects existing objects produces object IDs. A time-range tool produces an
explicit region. A region does not continuously recalculate object membership
unless an operation requests that calculation.

The initial `ProjectSelectionRegion` can be a typed variant of these region
types:

| Region | Coordinates and scope |
| --- | --- |
| `TimelineSelectionRegion` | Project seconds and an explicit track scope |
| `MidiNoteSelectionRegion` | Clip-content ticks, a clip scope, and optional pitch/group/channel constraints |
| `MidiAutomationSelectionRegion` | Clip-content ticks, a controller-lane address, and an optional value interval |

These types describe coordinate systems and constraints; they share one
selection service. Scope must distinguish all applicable tracks from an explicit
set of tracks without relying on an ambiguous empty list convention.

MIDI time coordinates refer to source content. Trimming a clip must not silently
redefine their origin. Document queries convert those coordinates to project
time using clip placement, source offset, and the applicable tempo mapping.
Moving a clip can therefore change a region's project-time projection while its
stored clip-content coordinates remain unchanged.

Controller-lane information describes what a region covers. It does not identify
individual automation points. Per-note associations must refer to note identity
rather than a mutable note number alone.

Region updates reject invalid coordinates, including nonfinite values and
reversed intervals. Zero-length time intervals can represent a point selection.
The exact region structures and endpoint conventions need to be specified before
implementation.

The bounding interval of selected objects is a derived query. It must not become
the stored identity of a selection, and it must not turn object selection into an
explicit range selection.

## Selection contexts and service

One document owns a collection of selection contexts. A timeline and two
piano-roll windows can retain three independent selections while referring to
the same document objects. Contexts express independent interactions; they do
not create separate content models or separate item identities.

The following is a schematic public surface in `uapmd-data`. Supporting handle,
result, and listener types remain to be finalized.

```cpp
class ProjectSelectionService {
public:
    SelectionContextId createContext();
    void removeContext(SelectionContextId context);

    ProjectSelection snapshot(SelectionContextId context) const;

    SelectionUpdateResult replace(
        SelectionContextId context,
        ProjectSelection selection);
    void clear(SelectionContextId context);

    void setActiveContext(
        std::optional<SelectionContextId> context);
    std::optional<SelectionContextId> activeContext() const;

    SelectionListenerToken addListener(
        ProjectSelectionListener& listener);
    void removeListener(SelectionListenerToken token);
};
```

The service validates object references and regions against document queries.
An invalid replacement fails without partially applying the supplied selection.
Additive and toggle interactions can construct replacements using the same API.

Listeners receive selection changes, active-context changes, and context
removal. A selection-change notification contains the context ID and the previous
and current snapshots. Replacing selection with an identical value emits no
notification. Changing the active context is observable even when the selections
involved contain the same objects.

The application reports the active host context. Window focus handling remains
in the application; opening or focusing a plugin view must not discard the host
selection it should display.

Closing an editor removes its selection context. The underlying shared MIDI
model and its identities remain alive with the document content. Project
replacement clears selections and invalidates contexts whose scopes no longer
exist; stale handles must not silently refer to a new context.

## Continuous edits, events, and history

Access and notification delivery occur on the model thread. The selection service
is not an audio-thread API. Its callbacks must not introduce blocking work into
audio processing.

Selection notifications are separate from `ProjectDocumentEventDispatcher` edit
events. Selection changes do not advance `ProjectRevision`, create history
entries, or mark the project dirty. The existing command/history machinery
continues to manage authored edits.

Moving a group of selected clips, notes, or automation points preserves their
selection throughout the gesture:

```text
Select objects       -> selection notification
Move/update objects  -> document notifications
Finish edit gesture  -> finish the history step
```

Changes to selected objects' geometry or values do not by themselves emit
`selectionChanged`. Consumers that need current geometry subscribe to document
changes as well as selection changes, then resolve the selected IDs again.

An explicit region can be adjusted continuously by publishing successive
selection replacements during a drag. The API does not impose publication only
on mouse release. Cancelling the gesture restores its initial selection
snapshot. Selection gestures remain outside document undo history.

Document transactions reconcile selection references against their final state.
An internal remove/reconstruct sequence that preserves an object's identity must
not transiently deselect it. Completed deletion prunes that object from every
affected context. Undo restores the object's identity without automatically
reselecting it.

Reconciliation and notifications must expose consistent post-mutation state.
Consumers must not observe intermediate reconstruction or failed changes as a
completed selection update. Exact ordering between document batches and
selection notifications must be specified with the implementation.

## Shared editable MIDI model

Extract the authored note and automation representation and its identity handling
from `PianoRollSession` into a shared MIDI clip model in `uapmd-data`. Keep window
state, input gestures, and application clipboard interaction in the application
layer.

The shared model retains:

- Stable note and automation-point IDs.
- Relationships between notes and their associated automation.
- Authored timing and controller information.
- The association between logical objects and their encoded UMP messages.

A note is a logical object whose encoding can include note-on and note-off
messages. An automation point is a logical editable event. Neither identity
depends on the number of words in its UMP representation. Raw word indexes are
encoding details and can change after sorting, translation, or reconstruction.

The writer must carry logical identities through such reconstruction explicitly.
Source reloads must resolve to the shared model rather than recreate an editor's
private identity space after every change.

MIDI snapshots retained by commands and `ProjectClipFragment` need to carry
identity information so undo can restore it. Editor commits through the current
`replaceMidiClipContent()` path must migrate to identity-bearing content. An
import of unrelated raw UMP is an explicit content replacement. Matching objects
heuristically by timestamp, pitch, controller value, or raw index cannot provide
the identity guarantee.

The shared model must retain unedited UMP content as well; introducing selectable
notes and automation points must not discard other messages during a round trip.
Playback source nodes consume the resulting authored content or published
snapshots without owning editor selections.

## Document API and engine boundary

Introduce a document entry point in `uapmd-data`, retaining
`ProjectDocumentView` as the read-only inspection contract. A tentative interface
is:

```cpp
class ProjectDocument {
public:
    virtual ~ProjectDocument() = default;

    virtual const ProjectDocumentView& view() const = 0;
    virtual ProjectDocumentEventSource& events() = 0;
    virtual ProjectSelectionService& selections() = 0;
    virtual const ProjectSelectionService& selections() const = 0;

    virtual ProjectCommands& commands() = 0;
    virtual ProjectCommandManager& commandManager() = 0;

    virtual void beginTransaction() = 0;
    virtual void endTransaction() = 0;
};
```

The document API needs queries for resolving selectable objects, inspecting their
type and parentage, and projecting explicit regions into project time. Existing
typed snapshots should be extended as needed. The selection service itself does
not need a switch over every object type or engine runtime pointers.

The `ProjectCommands` interface can move from `uapmd-engine` to `uapmd-data`,
alongside the data-only addresses and persisted settings it needs. Engine-backed
command implementations remain in `uapmd-engine`. The document transaction guard
can move to `uapmd-data` and operate on `ProjectDocument`.

The document API should expose `commandManager()` explicitly; the current
`commands().history()` returns a `ProjectCommandManager`, so those names should be
reconciled during migration rather than introducing another history mechanism.

| `uapmd-data` responsibilities | `uapmd-engine` responsibilities |
| --- | --- |
| Document inspection and identity contracts | Runtime track and plugin lookup |
| Selection storage, contexts, and notifications | Transport integration and audio rendering |
| Shared editable MIDI content and logical identities | Playback snapshot publication and source-node realization |
| Document events and command/history abstractions | Engine-backed command execution and resource preparation |
| Authored metadata and time-mapping value types | Latency, freeze behavior, and live graph replacement |

Initially, the engine can continue owning the document's lifetime and existing
track/clip storage. Concrete selection storage and the shared MIDI model are
implemented in `uapmd-data`; an engine-backed document composes those services.
`TimelineFacade` exposes the document and forwards legacy accessors during
migration. This does not require moving the full `TimelineFacade` into
`uapmd-data`.

Move the value types currently nested in `TimelineFacade::MasterTrackSnapshot`
to `uapmd-data`, so document consumers can inspect tempo/signature metadata
without naming the engine facade. Split document identity queries from
`ProjectAddressBook` operations that return live `SequencerTrack*` or resolve
runtime plugin instances.

Live project load/save orchestration can remain in the engine initially:
serializing authored data and reconstructing live tracks/plugins are distinct
responsibilities. Relocating all authoritative authored storage is a later
refactor, not a prerequisite for delivering selection.

## ARA projection

ARA consumes the active host selection context through the document API.

| Document selection | ARA representation |
| --- | --- |
| Selected clip | Playback-region reference |
| Selected track | Region-sequence reference |
| Selected note or automation point | Containing playback region |
| Explicit region | Applicable sequences and a project-time interval |
| Multiple explicit temporal regions | Their enclosing project-time interval |
| Object-only selection | No explicit time range |
| Empty selection | Empty `ARAViewSelection` |

Resolve and deduplicate references while preserving relevance order. ARA has no
individual-note or automation-point field in `ARAViewSelection`. Projecting those
objects to their containing clip does not turn them into an explicit range. The
original document selection remains available in full to other consumers.

Use document time-mapping queries when projecting clip-content regions. The
adapter must refresh its projection after document changes that affect ownership
or coordinate conversion, even when selection membership is unchanged. Examples
include moving a clip, changing tempo, or changing source offset.

Selection itself does not start an ARA editing cycle. The SDK permits editor-view
notifications inside editing cycles; sending them after model synchronization is
a host scheduling choice to ensure references and mappings are current.

Send the current projection during view setup or reopening and update visible
views. The SDK limits these view notifications to a displayed UI or its setup
phase. Rebinding or rebuilding ARA objects requires resolving their references
again before forwarding the current selection.

The SDK-backed controller and alternate AAP backend need separate capability
checks. A selection transport must not be assumed merely because a backend
supports document edits or advertises an editor role.

## Implementation sequence

1. Establish the document API, object identity contract, and region coordinate
   definitions in `uapmd-data`. Introduce the engine bridge and retain compatible
   facade accessors while callers migrate.
2. Extract shared editable MIDI content. Preserve note and automation identities
   through UMP reconstruction, fragments, command snapshots, and undo/redo.
3. Implement the common selection service and migrate timeline, piano-roll, and
   automation editing to its contexts. Reflect API-driven changes in editor
   rendering as well as publishing user interactions.
4. Connect ARA selection projection, view lifecycle, and relevant document
   changes. Continue migrating document-facing facade accessors independently
   from audio/runtime integration.

Validation should cover grouped clip moves, note and automation drags, mixed
selection, UMP sorting/rebuilding, copy/paste identities, deletion and undo,
content replacement, multiple contexts, project replacement, and ARA projection.
Verify that selection alone leaves revisions, dirty state, and history
unchanged. Build with an existing configured build directory; this plan does not
require CMake reconfiguration or prescribe new automated tests.

## Details to settle during implementation

- Exact object-resolution queries and the identity-bearing MIDI snapshot format.
- Region endpoint conventions, controller-lane addresses, and scope types.
- Context and listener lifetime handling, including stale-handle rejection.
- Reentrant updates and ordering between document batches, selection
  reconciliation, and selection notifications.
- Whether note and automation IDs should eventually be persisted across
  save/reopen.
- Selection forwarding capabilities of the alternate AAP backend.

These details refine the contracts above; they should not reintroduce editor
boundaries into the common selection representation.
