# Step Sequencer

## Status

The editor writes expanded MIDI notes with Flex Data loop and grid metadata,
restores the pattern from those markers, and warns before opening unmarked
clips. Sections describing possible API work and long-term features remain
design notes.

## Goal

Add a step sequencer editor for MIDI 2.0 clips. The first version is an
alternative editing view for an existing MIDI clip, rather than a new
real-time pattern playback engine or a new first-class project object.

The editor should be useful over a whole MIDI 2.0 clip and should support the
usual step-sequencer workflow: rhythmic columns, note toggling, velocity and
gate editing, pattern expansion, and transport-loop auditioning.

## MVP scope

- Display a MIDI clip as a fixed grid of musical steps.
- Support selectable divisions such as quarter, eighth, sixteenth, and
  thirty-second notes.
- Support selectable pattern lengths, for example 16, 32, or 64 steps.
- Toggle notes on and off, initially with a selectable MIDI pitch or pitch
  rows.
- Edit velocity and gate length.
- Show the transport playhead.
- Use the existing global transport loop to audition the clip's timeline
  range.
- Provide an `Expand by N` operation that bakes the pattern into N
  repetitions.
- Commit edits as ordinary MIDI UMP events through the existing MIDI clip
  replacement and history path.

The transport loop is global and may therefore loop other tracks as well.
That is acceptable for the MVP. A per-clip playback loop is outside this
initial design.

## Interoperable representation

The MIDI stream remains fully expanded and independently playable:

```text
[pattern events] [repetition 2] [repetition 3] ... [repetition N]
```

This means another MIDI-capable application can open the project or exported
clip without understanding UAPMD's step-sequencer convention. It will simply
play the baked repetitions as ordinary MIDI.

UAPMD adds a hint to the UMP stream using Flex Data Metadata Text. The marker
text contains only a stable semantic identifier, for example:

```text
uapmd.step-loop-end:v1
```

The marker's event timestamp is the authoritative end of the first reusable
pattern cycle. The text must not contain absolute tick values, DCTPQ, BPM,
step count, or repetition count.

For example:

```text
pattern events
loop-end marker at tick L
repetition 2
optional boundary marker at tick 2L
repetition 3
optional boundary marker at tick 3L
```

The editor writes a boundary marker after every cycle, including the final
cycle, so empty patterns and trailing silent repetitions retain their length.
The first loop-end marker is authoritative for the pattern length. Older clips
without later markers use the expanded event extent to infer repetitions.

A separate `uapmd.step-grid:v1` marker at the end of the first step records the
grid spacing by timestamp. This disambiguates sparse and empty patterns without
embedding tick values in text. Both marker types use Flex Data Metadata Text,
Unknown status, with channel addressing to retain the edited group and channel.
Step count is the loop timestamp divided by the grid timestamp; repetition
count is the final boundary timestamp divided by the first boundary timestamp.
Loop-only clips use the finest supported grid that fits within 128 steps.

## DCTPQ changes

The loop boundary is represented by the timestamp of the Flex Metadata event,
not by a number embedded in its text. Therefore a DCTPQ/tick-resolution
change can rescale the marker timestamp together with all other event
timestamps while leaving the metadata text unchanged.

Any tick-rescaling or UMP-reconstruction path must preserve Flex Metadata
messages as complete atomic UMP messages and must rescale their parallel tick
timestamps. Marker messages must not be overlooked merely because they are not
musical note or controller events.

## Loading and editing behavior

When UAPMD finds a valid `uapmd.step-loop-end:v1` marker:

1. Use the timestamp of the first marker as the pattern length.
2. Present the events before that timestamp as the editable pattern.
3. Retain the full baked event stream as the actual clip source.
4. Treat later events as baked repetitions.
5. Optionally validate later boundary markers against the inferred pattern
   length.

The repeated events should not be physically deleted from the loaded MIDI
source. The step editor should discard them only from its working projection.
This preserves the clip's playable duration and interoperability.

When a step edit is applied, the editor should regenerate the complete baked
stream from the editable pattern and its repetition count, then call the
existing MIDI clip-content replacement path. This should remain one undoable
clip-content operation.

If no valid marker exists, the step editor warns that the clip is not recognized
as originating from the step sequencer. The user can close without changes or
open anyway, acknowledging that subsequent edits apply immediately and may
discard data the editor cannot represent. Opening alone must leave the source
untouched. Unrelated Flex Data text and incomplete marker messages do not count
as step-sequencer metadata.

Edits replace the old recognized marker packets instead of accumulating them.
Only notes before the first loop boundary populate the working pattern; edits
regenerate the baked repetitions. Unsupported or contradictory marker settings
also trigger the warning. Clips saved before metadata writing was implemented
remain unmarked and trigger the warning until the user opens and edits them.

## `Expand by N`

`Expand by N` means “make the total baked sequence exactly N repetitions of
the current pattern.” The UI should show both the pattern length and the
resulting expanded length to avoid ambiguity:

```text
Pattern length: 16 steps
Repetitions: 4
Expanded length: 64 steps
```

The loop-end marker remains at the end of the first pattern cycle. The
repetition count is derived from the expanded event extent or maintained as
editor state; it is not required in the marker text.

The normal step-sequencer view edits the base pattern. A later feature may
provide an explicit command such as `Make Repetitions Independent` for users
who want to edit later repetitions separately.

## Event ownership and preservation

The editor should preserve non-note UMP content where possible, including
tempo/time-signature information, channel and UMP group, MIDI 2.0 note
attributes, and relevant automation.

The first implementation should define clearly which events inside the
pattern range it owns. A reasonable initial policy is that it owns note events
and preserves other event types without offering step-level editing for them.
Replication of controller, NRPN, pitch-bend, and per-note automation should
be specified before those events are expanded automatically.

## Playback filtering

Flex Metadata Text events are persisted in the MIDI clip but are authoring
information. MIDI clip playback consumes this status bank instead of forwarding
it to instrument plugins. This check does not allocate or lock on the audio thread;
other Flex Data status banks retain their existing playback behavior.

## Possible API work

The builtin editor can initially use the existing clip-editor integration and
MIDI replacement callback. If third-party clip-editor addins are expected to
implement step editing, the addin API should expose a reusable service for:

- reading grouped UMP events and tick timestamps;
- recognizing UAPMD step-loop metadata;
- replacing MIDI clip content with one history-aware operation; and
- requesting transport-loop auditioning for a clip range.

The initial feature should not introduce a new step-pattern project data model
or a per-clip real-time looping engine.

## Long-term boundary

This design deliberately stops short of first-class pattern sequencing.
Probability, ratchets, swing, pattern chaining, independent per-clip looping,
and non-destructive pattern instances would require a separate project and
playback design. The baked-MIDI plus metadata-hint approach leaves room for
those features without making them prerequisites for the first editor.
