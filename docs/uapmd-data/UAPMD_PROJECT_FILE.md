# UAPMD Project File Format Documentation (AI slop)

## Overview

UAPMD project files are JSON-based documents that define a timeline-based audio/MIDI project structure. The format supports multiple tracks, each containing clips (audio or MIDI) and optional audio graph state.

This document describes the project structure and its references to graph files.
The graph payload is specified separately in the
[Audio Graph File Format Specification](AUDIO_GRAPH_FILE.md), which describes the
currently implemented graph format.

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
    how the application saves every graph; see [Graph Reference Object](#graph-reference-object)
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

### Graph Reference Object

The current writer stores this project-level reference in `track.graph` (also
applicable to `master_track.graph`):

```json
{
  "graph_type": "",
  "external_file": "graphs/track_0.graph.json"
}
```

- `graph_type` identifies the graph provider; see the standalone specification's
  [Current Implemented Format](AUDIO_GRAPH_FILE.md#current-implemented-format).
- `external_file` locates the graph payload. Relative paths are resolved against
  the project directory, as are plugin state paths inside that payload.

The referenced file follows the [Audio Graph File Format Specification](AUDIO_GRAPH_FILE.md).
The reference object and the track's `volume` are part of this project format,
not fields that other enclosing formats must adopt. Legacy inline `plugins[]`
records use the plugin fields described in that specification.

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

## Complete Example

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

For the referenced graph payload, see the standalone specification’s
[Linear Chain Example](AUDIO_GRAPH_FILE.md#linear-chain-example).

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

Clip positions and durations are stored in samples; marker and audio warp offsets
are stored in seconds. The current project writer does not emit a dedicated
project sample-rate field. The loader uses the host sample rate when converting
sample positions to time.

### File Path Resolution

- Absolute paths are used as-is
- Relative paths are resolved relative to the project file location
- Missing files should be handled gracefully (warn user, allow re-linking)

### Supported MIDI Format

The `.midi2` files referenced in clips should conform to the **MIDI 2.0 Clip File Specification** (M2-116-U v1.0). It is distinct from standard MIDI file version 1.0 (SMF1).
