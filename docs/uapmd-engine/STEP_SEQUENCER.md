# Step Sequencer (AI slop)

## Status

Design note only. This document records an initial design discussion and is
intentionally marked as AI-generated working material. It may be removed or
replaced when the design changes significantly.

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

Boundary markers after the first cycle are optional. They can help with
validation and recovery, but the first loop-end marker is the authoritative
one.

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

If no marker exists, the whole clip is treated as one ordinary pattern. If a
marker is malformed or contradictory, UAPMD should leave the clip untouched
and fall back to ordinary MIDI editing behavior.

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

Flex Metadata events are persisted in the MIDI clip but are internal authoring
metadata. They should normally not be forwarded to instrument plugins as
musical track output. The playback path should recognize and consume these
metadata messages, or otherwise apply an explicit policy that prevents
internal markers from reaching ordinary instrument processing.

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
