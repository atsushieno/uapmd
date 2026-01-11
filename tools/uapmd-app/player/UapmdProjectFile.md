# UAPMD Project File Format Documentation

## Overview

UAPMD project files are JSON-based documents that define a timeline-based audio/MIDI project structure. The format supports multiple tracks, each containing clips (audio or MIDI) and plugin processing graphs.

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

## Plugin Graph Object

Defines an audio processing chain for a track.

### Schema

```json
{
  "external_file": "/path/to/graph.filtergraph",
  "plugins": [
    {
      "plugin_id": "com.example.reverb",
      "format": "VST3",
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
            "state_file": "/presets/vocal_eq.vstpreset"
          },
          {
            "plugin_id": "com.fabfilter.proc2",
            "format": "VST3",
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

