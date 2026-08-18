# UAPMD Project File Format Documentation (AI slop)

## Overview

UAPMD project files are JSON-based documents that define a timeline-based audio/MIDI project structure. The format supports multiple tracks, each containing clips (audio or MIDI) and optional audio graph state.

This document currently serves two purposes:

- it documents the currently implemented on-disk project and graph format that the codebase can already load and save today;
- it records the draft generic graph schema that new graph work should target.

The currently implemented format has these important properties:

- `track.volume` persists the implicit track/master fader even when no audio graph exists;
- `track.graph` is optional and may be omitted entirely;
- when a graph is saved, the current writer emits `track.graph` only as a reference to an external
  graph file; an inline `plugins[]` array is still read, but is no longer written;
- tracks, clips and plugin nodes carry persistent identity (`id`, and `node_id` for plugin nodes),
  and readers mint positional identifiers for projects written before that identity existed;
- the default runtime fader node `builtin:track_gain` is not serialized in `graph.nodes[]`;
- external DAG graph data uses `node_id` endpoint identifiers when newly written;
- older DAG graph data that still uses `plugin_index` remains readable for backward compatibility.

The generic graph schema is intended to describe common DAW-style audio graphs without baking UAPMD-specific DSP node names into the interchange format. Utility nodes should therefore be identified by public semantics, currently based on Web Audio API node concepts such as `GainNode`, `ChannelMergerNode`, and `ChannelSplitterNode`.

## File Structure

The project file is a JSON object with the following top-level structure:

```json
{
  "settings": { /* project-level settings */ },
  "tracks": [ /* array of track objects */ ],
  "master_track": { /* single track object */ }
}
```

### Fields

- **`settings`** (object, optional): Project-level settings
  - Each member is one named setting. Values are stored as parsed JSON rather than as strings,
    so a setting value may be of any JSON type
  - Written only when the project has at least one setting
- **`tracks`** (array, optional): Ordinary tracks, written only when the project has at least one
- **`master_track`** (object, optional): The master track; see Master Track

## Track Object

Each track represents an independent timeline with its own clips and optional audio graph state.

### Schema

```json
{
  "id": "track_0",
  "volume": 1.0,
  "muted": true,
  "solo": true,
  "graph": { /* graph object */ },
  "markers": [ /* array of marker objects */ ],
  "clips": [ /* array of clip objects */ ]
}
```

### Fields

- **`id`** (string, optional): Stable identity for this track
  - Preserved across save/load so that the track can be addressed by identity
    rather than by its position in the `tracks` array
  - Omitted by writers that predate persistent identity. Readers mint
    `"track_N"` (or `"master_track"`) for tracks that lack it, matching the
    positional identifier those writers used
  - Distinct from the anchor identifiers described under Clip Object, which
    address objects only within a single file
- **`volume`** (number, optional): Track/master fader gain as a linear scalar
  - Default is `1.0`
  - Recommended UAPMD-supported range is `0.0 .. 8.0`
  - This is the persisted source of truth for the implicit per-track/master fader
- **`muted`** (boolean, optional): Track mute state
  - Written only when the track is muted, so an absent field means not muted
- **`solo`** (boolean, optional): Track solo state
  - Written only when the track is soloed, so an absent field means not soloed
- **`graph`** (object, optional): Defines the audio graph for this track
  - The field may be omitted when the track does not need explicit graph topology or plugin state
  - The current writer emits it only when the graph carries an external file reference, which is
    how the application saves every graph; see Current Implemented Graph Object
- **`markers`** (array, optional): Track-level markers
  - Uses the same marker object as clip `markers`; see Marker Object
- **`clips`** (array, optional): List of audio or MIDI clips on this track's timeline

### Track Volume Persistence

The implicit per-track and master-track fader is stored only in the top-level track object's `volume` field.

It is intentionally not serialized as a graph node in `graph.nodes[]`, even though the runtime currently realizes that fader using an internal `webaudio:GainNode` with node id `builtin:track_gain`.

`graph.nodes[]` is reserved for graph-authored nodes that are part of the authored topology rather than the default track/master fader.

### Current Graph Persistence Behavior

Today a track can persist audio processing state in one of these ways:

- no `graph` field at all
  - valid when only `volume` and clips are needed
- an external graph file referenced through `graph.external_file`
  - this is what the current writer produces whenever a graph is saved, for linear chains as well
    as DAG topologies
  - the external graph payload may contain `plugins[]`, generic authored `nodes[]`, and DAG `connections[]`
- a simple inline graph object with `plugins[]`
  - still accepted by the reader, but no longer written: it appears in projects that were saved
    before graphs were always externalized

When DAG `connections[]` are written by current code, endpoint objects use `node_id`. Readers still accept deprecated `plugin_index` endpoint data from older project files.

## Clip Object

Clips represent either audio files or MIDI data positioned on the timeline.

### Schema

```json
{
  "id": "track_0_clip_0",
  "anchor": "track_0",
  "position_samples": 48000,
  "duration_samples": 192000,
  "file": "/path/to/audio.wav",
  "mime_type": "audio/wav",
  "clip_type": "audio",
  "markers": [ /* array of marker objects */ ],
  "audio_warps": [ /* array of warp point objects */ ]
}
```

### Fields

- **`id`** (string, optional): Stable identity for this clip
  - Preserved across save/load so that the clip can be addressed by identity
    rather than by its position in the `clips` array
  - Omitted by writers that predate persistent identity. Readers mint
    `"track_N_clip_M"` for clips that lack it
  - Independent of `anchor`: identity survives reordering, whereas anchor
    identifiers are regenerated from array position on every write

- **`anchor`** (string, optional): ID of the anchor point for positioning
  - If omitted or `null`, position is absolute (relative to track start at sample 0)
  - Valid anchor ID examples:
    - `"track_N"` - Anchor to the start of track N (sample 0 of that track)
    - `"track_N_clip_M"` - Anchor to the start of clip M on track N
    - `"master_track"` - Anchor to master track start
    - `"master_clip_M"` - Anchor to clip M on master track

- **`position_samples`** (integer, required): Position in samples
  - If `anchor` is null/omitted: absolute position from track start
  - If `anchor` is set: offset in samples from the anchor point
  - Can be negative to position before the anchor point

- **`file`** (string, optional): Path to the clip file
  - Absolute or relative path to audio/MIDI file
  - Supported formats:
    - Audio: any format supported by the audio file reader (WAV, AIFF, FLAC, etc.)
    - MIDI: `.midi2` files (SMF2 Clip format per M2-116-U v1.0 specification)

- **`mime_type`** (string, optional): MIME type of the clip file
  - If omitted, type is inferred from file extension
  - Common values:
    - `"audio/wav"` - WAV audio
    - `"audio/aiff"` - AIFF audio
    - `"audio/flac"` - FLAC audio

- **`clip_type`** (string, optional): Kind of clip
  - Written when known; current values are `"audio"` and `"midi"`
  - Selects whether the MIDI-only fields below apply

- **`duration_samples`** (integer, optional): Clip length in samples
  - Written only when greater than zero

- **`markers`** (array, optional): Markers within this clip
  - See Marker Object

- **`audio_warps`** (array, optional): Warp points that time-stretch this clip
  - See Audio Warp Point Object

- **`tick_resolution`** (integer, optional): Ticks per quarter note for MIDI content
  - Written only when `clip_type` is `"midi"`

- **`nrpn_to_parameter_mapping`** (boolean, optional): Map incoming NRPN messages to plugin parameters
  - Written only when `clip_type` is `"midi"` and the mapping is enabled, so an absent field means disabled

## Marker Object

Markers appear both in a clip's `markers` array and in a track's `markers` array, with the same shape.

```json
{
  "id": "marker_0",
  "position_offset_seconds": 1.5,
  "reference_type": "clip_start",
  "reference_clip_id": "track_0_clip_0",
  "reference_marker_id": "marker_0",
  "name": "Chorus"
}
```

- **`id`** (string, optional): Marker identifier, written when non-empty
- **`position_offset_seconds`** (number, required): Offset from the reference point, in seconds
  - Older projects may instead carry `position_offset_samples`, which readers convert
- **`reference_type`** (string, required): What the offset is measured from
  - One of `"manual"`, `"clip_start"`, `"clip_end"`, `"clip_marker"`, `"master_marker"`
- **`reference_clip_id`** (string, optional): Referenced clip, written when non-empty
- **`reference_marker_id`** (string, optional): Referenced marker, written when non-empty
- **`name`** (string, optional): Human-readable label, written when non-empty

## Audio Warp Point Object

```json
{
  "offset_seconds": 2.0,
  "speed_ratio": 1.25,
  "reference_type": "clip_marker",
  "reference_marker_id": "marker_0",
  "marker_id": "marker_0"
}
```

- **`offset_seconds`** (number, required): Offset of this warp point, in seconds
  - Older projects may instead carry `clip_position_offset_samples`, `offset_samples`,
    `source_offset_samples` or `source_position_samples`, which readers convert
- **`speed_ratio`** (number, required): Playback speed multiplier applied from this point
- **`reference_type`** (string, required): Same values as the marker `reference_type`
- **`reference_clip_id`** (string, optional): Referenced clip, written when non-empty
- **`reference_marker_id`** (string, optional): Referenced marker, written when non-empty
- **`marker_id`** (string, optional): Legacy duplicate of `reference_marker_id`
  - Written only when `reference_type` is `"clip_marker"`, so that older readers still resolve the reference

## Generic Graph Schema Draft

This is the proposed direction for future graph serialization work. It is not yet the complete description of the currently written project format.

### Goals

- represent plugin nodes and utility DSP nodes in one common graph model;
- avoid plugin-index-based endpoint addressing;
- allow node kinds that are not tied to UAPMD branding;
- preserve room for future DAW interoperability;
- keep version 1 focused on plain values and explicit connections.

### Top-Level Graph Object

```json
{
  "schema_version": 1,
  "graph_type": "urn:uapmd-graph:generic/dag/v1",
  "nodes": [],
  "connections": []
}
```

### Fields

- **`schema_version`** (integer, required): Version of the generic graph schema
- **`graph_type`** (string, required): Graph format identifier
- **`nodes`** (array, required): Graph-local node definitions
- **`connections`** (array, required): Directed edges between node ports

### Generic Graph Persistence Notes

- The implicit track/master fader is not serialized in `nodes[]`; use the parent track object's `volume` field instead.
- Graph-authored built-in nodes, including authored `webaudio:GainNode` instances that are not the implicit default fader, are serialized in `nodes[]`.

### Node Object

All runtime nodes share one common envelope:

```json
{
  "id": "track_gain_0",
  "type": "webaudio:GainNode",
  "display_name": "Track Volume",
  "options": {},
  "parameters": {
    "gain": 1.0
  },
  "state": {},
  "metadata": {}
}
```

### Node Fields

- **`id`** (string, required): Stable graph-local node identifier
- **`type`** (string, required): Namespaced node type
- **`display_name`** (string, optional): Human-readable label
- **`options`** (object, optional): Structural configuration for the node
- **`parameters`** (object, optional): Plain runtime parameter values
- **`state`** (object, optional): Persisted opaque or type-specific state
- **`metadata`** (object, optional): Non-audio hints such as editor metadata

### Node Type Naming

The `type` field should use a namespace-like prefix to make node semantics explicit.

Examples:

- `webaudio:GainNode`
- `webaudio:ChannelMergerNode`
- `webaudio:ChannelSplitterNode`
- `plugin:vst3`
- `plugin:clap`
- `plugin:au`
- `plugin:lv2`

### Connection Object

```json
{
  "id": "c0",
  "kind": "audio",
  "source": {
    "node_id": "plugin_0",
    "port": "out:0"
  },
  "target": {
    "node_id": "track_gain_0",
    "port": "in"
  }
}
```

### Connection Fields

- **`id`** (string, required): Stable graph-local connection identifier
- **`kind`** (string, required): Connection kind. Version 1 uses `audio`
- **`source`** (object, required): Source endpoint
- **`target`** (object, required): Target endpoint

### Endpoint Object

```json
{
  "node_id": "graph:output",
  "port": "in:0",
  "channel": 0
}
```

### Endpoint Fields

- **`node_id`** (string, required): Node identifier or reserved pseudo-node identifier
- **`port`** (string, required): Symbolic port name such as `in`, `out`, `in:0`, or `out:1`
- **`channel`** (integer, optional): Optional per-channel selection for future fine-grained routing

### Reserved Pseudo-Nodes

The generic graph reserves pseudo-node identifiers for graph boundaries:

- `graph:input`
- `graph:output`

These allow the graph to represent track/master ingress and egress without inventing a separate endpoint type outside the node model.

### Example: Gain Node

```json
{
  "id": "track_gain_0",
  "type": "webaudio:GainNode",
  "display_name": "Track Volume",
  "parameters": {
    "gain": 1.0
  }
}
```

Notes:

- `parameters.gain` is a linear scalar gain value;
- UAPMD may choose to present it in the UI with a dB-oriented or exponential-feeling slider;
- the UI taper is host behavior and is not serialized;
- a recommended initial UAPMD-supported range is `0.0 .. 8.0`, default `1.0`.

### Example: Channel Merger Node

```json
{
  "id": "mono_to_stereo_0",
  "type": "webaudio:ChannelMergerNode",
  "options": {
    "number_of_inputs": 2
  }
}
```

### Example: Channel Splitter Node

```json
{
  "id": "stereo_split_0",
  "type": "webaudio:ChannelSplitterNode",
  "options": {
    "number_of_outputs": 2
  }
}
```

### Example: Plugin Node

```json
{
  "id": "plugin_0",
  "type": "plugin:vst3",
  "display_name": "SuperSynth",
  "plugin": {
    "plugin_id": "com.example.supersynth",
    "state_file": "states/plugin_0.bin"
  }
}
```

Plugin node notes:

- `plugin` is a type-specific payload for plugin-backed nodes;
- `plugin_id` is the plugin identifier used by the corresponding plugin format;
- `state_file` references persisted plugin state owned by the project;
- additional type-specific fields may be added later if required.

### Complete Example

This is the shape the current writer produces: persistent `id` on tracks and clips, and each
track's graph stored as an external graph file rather than an inline `plugins[]` array.

```json
{
  "tracks": [
    {
      "id": "track_0",
      "volume": 1.0,
      "graph": {
        "graph_type": "",
        "external_file": "graphs/track_0.graph.json"
      },
      "clips": [
        {
          "id": "track_0_clip_0",
          "position_samples": 0,
          "duration_samples": 480000,
          "file": "/audio/vocal_verse1.wav",
          "mime_type": "audio/wav",
          "clip_type": "audio"
        },
        {
          "id": "track_0_clip_1",
          "anchor": "track_0_clip_0",
          "position_samples": 480000,
          "file": "/audio/vocal_chorus.wav",
          "mime_type": "audio/wav",
          "clip_type": "audio"
        }
      ]
    },
    {
      "id": "track_1",
      "volume": 0.8,
      "muted": true,
      "clips": [
        {
          "id": "track_1_clip_0",
          "position_samples": 0,
          "file": "/midi/drums.midi2",
          "clip_type": "midi",
          "tick_resolution": 480
        }
      ]
    }
  ],
  "master_track": {
    "id": "master_track",
    "volume": 0.95,
    "graph": {
      "graph_type": "",
      "external_file": "graphs/master_track.graph.json"
    }
  }
}
```

The referenced `graphs/track_0.graph.json` holds the plugin chain itself:

```json
{
  "graph_type": "",
  "plugins": [
    {
      "node_id": "plugin:0",
      "plugin_id": "com.fabfilter.proq3",
      "format": "VST3",
      "display_name": "FabFilter Pro-Q 3",
      "state_file": "/presets/vocal_eq.vstpreset",
      "group_index": -1
    },
    {
      "node_id": "plugin:1",
      "plugin_id": "com.fabfilter.proc2",
      "format": "VST3",
      "display_name": "FabFilter Pro-C 2",
      "state_file": "/presets/vocal_comp.vstpreset",
      "group_index": -1
    }
  ]
}
```

## Implementation Notes

### Anchor Resolution

When reading a project file, anchors are resolved in two passes:

1. **First pass**: All tracks and clips are loaded, and anchor IDs are generated based on their position in the file
2. **Second pass**: Clip positions are updated to reference the actual anchor objects, with validation

This ensures that clips can reference anchors that appear later in the file.

### Anchor Validation

During the second pass, the reader validates all anchors. An anchor is considered **invalid** if:

1. **Anchor not found**: The specified anchor ID does not exist in the project
   - Example: `"anchor": "track_999"` when there are only 3 tracks

2. **Recursive reference**: The anchor creates a circular dependency
   - Example: Clip A anchors to Clip B, Clip B anchors to Clip C, Clip C anchors back to Clip A

**Behavior for invalid anchors:**
- A warning is printed to stderr describing the issue
- The clip with the invalid anchor is **removed** from the track
- The project continues loading with remaining valid clips

**Example warning messages:**
```
Warning: Invalid anchor 'track_5' in track 0 clip 2 - anchor not found. Clip will be removed.
Warning: Invalid anchor 'track_0_clip_1' in track 0 clip 0 - creates recursive reference. Clip will be removed.
```

### Sample Rate Considerations

All positions are specified in samples, not time units. When working with projects that may use different sample rates:

- Store sample rate metadata at the project level (future enhancement)
- Convert positions when importing/exporting between different sample rates
- UI should display both samples and time units (HH:MM:SS.mmm)

### File Path Resolution

- Absolute paths are used as-is
- Relative paths are resolved relative to the project file location
- Missing files should be handled gracefully (warn user, allow re-linking)

### Supported MIDI Format

The `.midi2` files referenced in clips should conform to the **MIDI 2.0 Clip File Specification** (M2-116-U v1.0). This is distinct from standard MIDI files:

- MIDI 1.0 SMF: `.mid` files (not currently supported in clip format)
- MIDI 2.0 Clip: `.midi2` files (SMF2 Clip format)
- MIDI 2.0 Container: Future format for complete project files with embedded clips

Note that we ONLY support `.midi2`. These formats explained above are for clarification.
