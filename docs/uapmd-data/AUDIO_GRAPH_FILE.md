# Audio Graph File Format Specification (AI slop)

## Scope and Status

This document describes JSON audio graph files independently of any enclosing project
format. A graph describes processing nodes, plugin state references, and routing;
it does not require tracks, clips, a timeline, or a particular archive layout.
Other project formats can reference this specification for their graph payloads.

This specification describes the format currently read and written by the UAPMD
graph providers. Compatibility behavior for older files is noted where relevant.

## Integration with an Enclosing Format

The enclosing format defines how graph files are located, how relative plugin
`state_file` paths are resolved, and how graph inputs and outputs connect to its
own processing model. A graph file does not prescribe a ZIP layout or require a
particular filename; UAPMD uses `.graph.json`.

For the UAPMD binding, see the project format's
[Graph Reference Object](UAPMD_PROJECT_FILE.md#graph-reference-object) and
[Track Volume Persistence](UAPMD_PROJECT_FILE.md#track-volume-persistence).
In that binding, relative state paths are resolved against the project directory,
not the graph file's directory. The implicit track/master fader is owned by the
project and excluded from authored graph nodes.

## Current Implemented Format

The root is a JSON object. The current writer emits `graph_type` and omits empty
collections. It does not emit `schema_version`.

| Field | Type | Meaning |
| --- | --- | --- |
| `graph_type` | string | Provider identifier: `""` for a simple linear chain, `"urn:uapmd-graph:common/graph/dag/v1"` for the full DAG provider. |
| `plugins` | array | Plugin records; array order defines the simple linear chain. |
| `nodes` | array | Authored generic node descriptors. |
| `connections` | array | Explicit DAG edges; a linear provider rejects a nonempty array. |
| `properties` | object | Provider-specific string values keyed by property name. |

### Plugin Records

The current writer stores plugin instances in `plugins[]`, separately from generic
`nodes[]`. Each record contains:

- `node_id`: stable node identifier; older records without it receive `plugin:N`,
  where N is the zero-based position in `plugins[]`.
- `plugin_id`: identifier in the plugin format.
- `format`: plugin format name, such as `VST3`.
- `display_name`: human-readable plugin name.
- `state_file`: reference to saved plugin state; may be empty.
- `group_index`: integer MIDI group selection, default `-1`.

### Generic Node Records

A `nodes[]` entry requires `node_id` and `type`. Optional fields are `display_name`,
`options`, `parameters`, `metadata`, and `plugin`. Values within `options`,
`parameters`, and `metadata` are scalars (boolean, integer, floating-point number,
or string). The optional `plugin` payload contains `format`, `plugin_id`, and
`state_file` strings. The current runtime writer stores plugin instances in
`plugins[]` instead and excludes the UAPMD-owned `builtin:track_gain` node.

For example, an authored gain node is represented as:

```json
{
  "node_id": "gain_0",
  "type": "webaudio:GainNode",
  "parameters": { "gain": 0.5 }
}
```

### Built-in Node Types

The built-in node registry supports these descriptors:

| `type` | Configuration | Meaning |
| --- | --- | --- |
| `webaudio:AnalyserNode` | No node-specific configuration fields. | Pass-through audio/event processing with time-domain and frequency analysis. |
| `webaudio:GainNode` | `parameters.gain` | Linear gain multiplier. |
| `webaudio:ChannelMergerNode` | `options.number_of_inputs` | Number of mono inputs combined into a multichannel output in a DAG. |
| `webaudio:ChannelSplitterNode` | `options.number_of_outputs` | Number of mono outputs split from a multichannel input in a DAG. |

For example, a two-input merger descriptor is:

```json
{
  "node_id": "merger_0",
  "type": "webaudio:ChannelMergerNode",
  "options": { "number_of_inputs": 2 }
}
```

The JSON reader and writer support the descriptor fields above. The current
runtime graph serializer captures node identity, type, display name, and gain
parameters for gain nodes; it does not capture merger/splitter options or generic
metadata from runtime nodes. These fields therefore do not all survive an
application load/save cycle. A `plugin` payload in `nodes[]` is parsed, but the
runtime graph builder skips it; plugin instances must be listed in `plugins[]`.

### Analyser Node

`webaudio:AnalyserNode` is serializable as a generic `nodes[]` descriptor and is
registered with the built-in node factory. For example:

```json
{
  "node_id": "analyser_0",
  "type": "webaudio:AnalyserNode",
  "display_name": "Output Spectrum"
}
```

The node supplies audio data to consumers such as a spectrum analyzer whose UI
must be rendered outside the realtime audio thread. During audio processing, it
passes audio and events through and publishes captured samples using a lock-free
snapshot. A consumer on a non-realtime thread reads that snapshot through
`getFloatTimeDomainData()` or requests frequency data through
`getFloatFrequencyData()`. Frequency analysis runs on the calling non-realtime
thread; the audio callback only captures samples. The consumer uses the returned
data to render its visualization without performing analysis or UI rendering in
the audio callback.

Its current implementation uses a fixed 256-sample analysis window and 128
frequency bins, averaging the channels of the first audio bus. It consumes no node-specific `options` or `parameters`; the Web Audio-style
name does not imply support for configurable FFT size or smoothing properties.

Serialization preserves the node descriptor, not captured samples or analysis
results. Those are transient runtime data. An analyser that belongs to a graph
can be serialized through the same generic-node path as other built-in nodes;
an analyser instantiated separately by the host is not automatically part of
that graph's JSON payload.

### DAG Connections

Each connection contains an integer `id`, a `bus_type` (`audio` or `event`), and
`source` and `target` endpoint objects:

```json
{
  "id": 1,
  "bus_type": "audio",
  "source": { "type": "graph_input", "node_id": "graph:input", "bus_index": 0 },
  "target": { "type": "plugin", "node_id": "plugin:0", "bus_index": 0 }
}
```

An endpoint contains `type` (`graph_input`, `graph_output`, or `plugin`),
`node_id`, and a zero-based `bus_index`. The boundary identifiers are
`graph:input` and `graph:output`. Readers infer `type` from `node_id` when the
former is absent, and default an absent `bus_index` to zero.
Older endpoints using `plugin_index` remain readable, but current writers use
`node_id` and do not emit `plugin_index`.

### Linear Chain Example

A standalone graph file containing a plugin chain:

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

### Complete DAG Example

This graph routes audio from the graph input through an authored gain node to
the graph output. The endpoint type `plugin` is also used for generic processing
nodes; `node_id` identifies the actual node.

```json
{
  "graph_type": "urn:uapmd-graph:common/graph/dag/v1",
  "nodes": [
    {
      "node_id": "gain_0",
      "type": "webaudio:GainNode",
      "parameters": { "gain": 0.5 }
    }
  ],
  "connections": [
    {
      "id": 1,
      "bus_type": "audio",
      "source": { "type": "graph_input", "node_id": "graph:input", "bus_index": 0 },
      "target": { "type": "plugin", "node_id": "gain_0", "bus_index": 0 }
    },
    {
      "id": 2,
      "bus_type": "audio",
      "source": { "type": "plugin", "node_id": "gain_0", "bus_index": 0 },
      "target": { "type": "graph_output", "node_id": "graph:output", "bus_index": 0 }
    }
  ]
}
```
