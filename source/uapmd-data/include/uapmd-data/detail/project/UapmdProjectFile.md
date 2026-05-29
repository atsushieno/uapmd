# UAPMD Project File Format Documentation

## Overview

UAPMD project files are JSON-based documents that define a timeline-based audio/MIDI project structure. The format supports multiple tracks, each containing clips (audio or MIDI) and audio processing graphs.

This document currently serves two purposes:

- it documents the legacy graph representation that the codebase can already load and save today;
- it records the draft generic graph schema that new graph work should target.

The generic graph schema is intended to describe common DAW-style audio graphs without baking UAPMD-specific DSP node names into the interchange format. Utility nodes should therefore be identified by public semantics, currently based on Web Audio API node concepts such as `GainNode`, `ChannelMergerNode`, and `ChannelSplitterNode`.

## File Structure

The project file is a JSON object with the following top-level structure:

```json
{
  "tracks": [ /* array of track objects */ ],
  "master_track": { /* single track object */ }
}
```

## Track Object

Each track represents an independent timeline with its own clips and audio processing chain.

### Schema

```json
{
  "graph": { /* plugin graph object */ },
  "clips": [ /* array of clip objects */ ]
}
```

### Fields

- **`graph`** (object, optional): Defines the audio plugin processing chain for this track
- **`clips`** (array, optional): List of audio or MIDI clips on this track's timeline

## Clip Object

Clips represent either audio files or MIDI data positioned on the timeline.

### Schema

```json
{
  "anchor": "track_0",
  "position_samples": 48000,
  "file": "/path/to/audio.wav",
  "mime_type": "audio/wav"
}
```

### Fields

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

## Generic Graph Schema Draft

This is the proposed direction for new graph serialization work.

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

```json
{
  "schema_version": 1,
  "graph_type": "urn:uapmd-graph:generic/dag/v1",
  "nodes": [
    {
      "id": "plugin_0",
      "type": "plugin:vst3",
      "display_name": "SuperSynth",
      "plugin": {
        "plugin_id": "com.example.supersynth",
        "state_file": "states/plugin_0.bin"
      }
    },
    {
      "id": "track_gain_0",
      "type": "webaudio:GainNode",
      "display_name": "Track Volume",
      "parameters": {
        "gain": 1.0
      }
    }
  ],
  "connections": [
    {
      "id": "c0",
      "kind": "audio",
      "source": { "node_id": "plugin_0", "port": "out:0" },
      "target": { "node_id": "track_gain_0", "port": "in" }
    },
    {
      "id": "c1",
      "kind": "audio",
      "source": { "node_id": "track_gain_0", "port": "out" },
      "target": { "node_id": "graph:output", "port": "in:0" }
    }
  ]
}
```

### Validation Expectations

Version 1 readers and writers should assume the following:

- node IDs are unique within a graph;
- connection IDs are unique within a graph;
- connection endpoints reference existing nodes or reserved pseudo-nodes;
- unknown node types may be preserved but do not have to be instantiated;
- unknown fields should be ignored where possible;
- parameter values are plain persisted values, not automation curves;
- channel routing behavior should be explicit in the graph representation rather than inferred from host-specific defaults.

### Non-Goals for Version 1

The following are intentionally out of scope for the first generic graph schema revision:

- sample-accurate parameter automation inside graph JSON;
- UI slider taper or editor behavior serialization;
- full Web Audio API behavioral compatibility;
- implicit browser-style channel interpretation rules;
- modulation/control-rate signal connections.

## Legacy Plugin Graph Object

The following legacy representation is still documented because current code can load and save it. New schema work should prefer the generic graph draft above.

Defines an audio processing chain for a track.

### Schema

```json
{
  "external_file": "/path/to/graph.filtergraph",
  "plugins": [
    {
      "plugin_id": "com.example.reverb",
      "format": "VST3",
      "display_name": "Studio Reverb",
      "state_file": "/path/to/state.vstpreset"
    }
  ]
}
```

### Fields

- **`external_file`** (string, optional): Path to external graph definition file
  - If specified, represents a complex graph (e.g., JUCE `.filtergraph` format)
  - When present, the `plugins` array may be empty or used for simple plugin list

- **`plugins`** (array, optional): Linear list of plugin nodes
  - Plugins are processed in array order (linear signal chain)
  - Each plugin object contains:

#### Plugin Node Object

```json
{
  "plugin_id": "com.example.plugin.id",
  "format": "VST3",
  "display_name": "Plugin Display Name",
  "state_file": "/path/to/state.vstpreset"
}
```

- **`plugin_id`** (string, required): Unique identifier for the plugin
  - VST3: Plugin UID string
  - AU: Component description identifier
  - LV2: Plugin URI
  - CLAP: Plugin ID string

- **`format`** (string, required): Plugin format
  - Supported values: `"VST3"`, `"AU"`, `"LV2"`, `"CLAP"`

- **`display_name`** (string, optional but recommended): Human-readable plugin name
  - Stored alongside `plugin_id` to make project files easier to inspect
  - Used when reloading projects as a fallback hint if the original `plugin_id` is no longer available (e.g., plugin was renamed or moved); see `docs/remidy/PLUGIN_ID_AND_CATALOG.md`

- **`state_file`** (string, optional): Path to plugin state/preset file
  - Format-specific preset/state file
  - Can be empty if using default plugin state

## Master Track

The master track is a special track that processes the final mix output. It follows the same structure as regular tracks but is defined as a single object rather than an array element.

```json
{
  "master_track": {
    "graph": { /* plugin graph for master bus */ },
    "clips": [ /* clips on master timeline */ ]
  }
}
```

## Positioning Examples

### Example 1: Absolute Positioning

Clip starts at 2 seconds (96000 samples at 48kHz) from track start:

```json
{
  "position_samples": 96000,
  "file": "/audio/intro.wav"
}
```

### Example 2: Track-Anchored Positioning

Clip starts 1 second after the beginning of track 0:

```json
{
  "anchor": "track_0",
  "position_samples": 48000,
  "file": "/audio/drums.wav"
}
```

### Example 3: Clip-Anchored Positioning

Clip starts immediately after another clip ends (assuming the anchor clip is 192000 samples long):

```json
{
  "anchor": "track_1_clip_0",
  "position_samples": 192000,
  "file": "/audio/verse.wav"
}
```

### Example 4: Negative Offset

Clip starts 0.5 seconds before the anchor point (crossfade scenario):

```json
{
  "anchor": "track_0_clip_2",
  "position_samples": -24000,
  "file": "/audio/transition.wav"
}
```

## Complete Example

```json
{
  "tracks": [
    {
      "graph": {
        "plugins": [
        {
          "plugin_id": "com.fabfilter.proq3",
          "format": "VST3",
          "display_name": "FabFilter Pro-Q 3",
          "state_file": "/presets/vocal_eq.vstpreset"
        },
        {
          "plugin_id": "com.fabfilter.proc2",
          "format": "VST3",
          "display_name": "FabFilter Pro-C 2",
          "state_file": "/presets/vocal_comp.vstpreset"
        }
      ]
      },
      "clips": [
        {
          "position_samples": 0,
          "file": "/audio/vocal_verse1.wav",
          "mime_type": "audio/wav"
        },
        {
          "anchor": "track_0_clip_0",
          "position_samples": 480000,
          "file": "/audio/vocal_chorus.wav",
          "mime_type": "audio/wav"
        }
      ]
    },
    {
      "graph": {
        "plugins": []
      },
      "clips": [
        {
          "position_samples": 0,
          "file": "/midi/drums.midi2"
        }
      ]
    }
  ],
  "master_track": {
    "graph": {
      "plugins": [
        {
          "plugin_id": "com.fabfilter.prol2",
          "format": "VST3",
          "display_name": "FabFilter Pro-L 2",
          "state_file": "/presets/mastering_limiter.vstpreset"
        }
      ]
    },
    "clips": []
  }
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
