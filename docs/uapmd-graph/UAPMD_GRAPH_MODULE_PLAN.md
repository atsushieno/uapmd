# UAPMD Graph Module Plan (AI slop)

## Status

- Implemented through Stage 7 for the current scope
- Schema direction documented in [source/uapmd-data/include/uapmd-data/detail/project/UapmdProjectFile.md](/Users/atsushi/sources/uapmd/source/uapmd-data/include/uapmd-data/detail/project/UapmdProjectFile.md)
- `uapmd-graph` module owns the graph runtime, node interfaces, registry, and all built-in node implementations
- `webaudio:GainNode` is fully integrated: created per-track by `SequencerEngine`, routed through both graph types, and controlled via the volume slider in uapmd-app
- Track/master fader persistence now uses top-level track JSON `volume` rather than depending on graph serialization
- Graph JSON now persists generic built-in node descriptors under `nodes[]` while retaining the existing `plugins[]` and DAG `connections[]` payloads
- The implicit per-track/master gain node (`builtin:track_gain`) is no longer serialized through graph `nodes[]`; `track.volume` is its only persisted source of truth
- Loading project graphs now recreates built-in runtime nodes from persisted descriptors for both simple and DAG graph providers
- DAG graph connections now serialize stable `node_id` endpoint identifiers, while the loader still accepts deprecated `plugin_index` fields for backward compatibility with older project files
- DAG runtime and editor now treat built-in nodes as addressable graph nodes by `node_id` instead of only as a post-graph special case
- UI and scripting surfaces now use generic graph-node terminology instead of treating all graph nodes as plugins

## Remaining Work

- Compatibility policy
  - keep the deprecated `plugin_index` reader fallback for backward compatibility with older project files
  - consider retiring that reader path only when old-project compatibility is no longer required
- Built-in node expansion
  - implement additional built-in graph node kinds beyond `webaudio:GainNode`
  - planned next targets include channel utilities such as `webaudio:ChannelMergerNode` and `webaudio:ChannelSplitterNode`
- Bus and layout semantics
  - refine built-in node input/output layout semantics so graph-authored utility nodes do not rely on coarse track-level assumptions
  - make channel-routing behavior clearer for future mixer/remapper-style nodes
- Additional regression coverage if needed
  - broaden coverage around generic built-in nodes and future channel-utility nodes as they are implemented

## Goal

Create a dedicated `uapmd-graph` module that owns audio graph runtime concepts and built-in graph nodes such as `webaudio:GainNode`, while keeping:

- `uapmd` focused on common types, MIDI, and plugin-hosting APIs;
- `uapmd-data` focused on project/graph serialization;
- `uapmd-engine` focused on sequencer behavior, plugin lifetime, function block mapping, and runtime policy.

## Motivation

The current graph implementation is plugin-centric and currently lives under `uapmd`. That makes it awkward to add independent graph nodes such as gain or channel remapping nodes, because:

- every runtime node is assumed to wrap `AudioPluginInstanceAPI`;
- graph serialization is shaped around plugin lists and plugin indices;
- graph editor and engine integration assume graph nodes are plugins;
- graph functionality is becoming large enough to deserve its own module boundary.

The `uapmd-graph` split is intended to make graph work tractable without moving more unrelated responsibilities into `uapmd-engine` or `uapmd-data`.

## Proposed Module Dependency Direction

```text
remidy
  ^
  |
uapmd
  ^
  |
uapmd-graph
  ^       ^
  |       |
uapmd-data uapmd-engine
```

Meaning:

- `uapmd-graph` depends on `uapmd`
- `uapmd-data` depends on `uapmd-graph`
- `uapmd-engine` depends on `uapmd-graph` and `uapmd-data`

## Responsibilities By Module

### `uapmd`

- common types such as `AudioProcessContext`
- plugin hosting APIs such as `AudioPluginInstanceAPI`
- MIDI device / function block support
- no ownership of built-in graph node implementations

### `uapmd-graph`

- graph runtime interfaces
- graph node runtime interfaces
- linear graph and full DAG graph implementations
- built-in graph node registry
- built-in node implementations such as `webaudio:GainNode`
- graph descriptor types shared by runtime and serialization layers

### `uapmd-data`

- JSON/project file readers and writers
- conversion between persisted graph JSON and `uapmd-graph` descriptor types
- no ownership of built-in DSP behavior

### `uapmd-engine`

- sequencer tracks and graph ownership at runtime
- plugin instance creation/destruction
- policy such as when default gain nodes are inserted
- MIDI-CI function block mappings
- plugin UMP routing into and out of plugin-backed graph nodes

## Important Design Constraint

`AudioGraph` is not completely independent from plugin routing, because the engine uses graph/plugin node lookup to:

- deliver incoming UMP to plugin-backed nodes;
- rewrite and dispatch outgoing UMP from plugin-backed nodes;
- connect plugin instances to MIDI-CI function block mappings by `instanceId`.

However, this does not mean the graph module should remain inside `uapmd`. It means `uapmd-graph` must support plugin-backed nodes as one node category, while keeping built-in nodes independent from MIDI-CI concerns.

## Proposed Directory Layout

```text
source/uapmd-graph/
  CMakeLists.txt
  include/uapmd-graph/uapmd-graph.hpp
  include/uapmd-graph/detail/CommonGraphTypes.hpp
  include/uapmd-graph/detail/AudioGraphNode.hpp
  include/uapmd-graph/detail/AudioGraphPluginNode.hpp
  include/uapmd-graph/detail/AudioGraph.hpp
  include/uapmd-graph/detail/AudioFullDAGraph.hpp
  include/uapmd-graph/detail/AudioGraphDescriptor.hpp
  include/uapmd-graph/detail/AudioGraphRegistry.hpp
  include/uapmd-graph/detail/AudioGraphBuiltInNodeFactory.hpp
  include/uapmd-graph/detail/builtin/BuiltInNodeTypes.hpp
  src/AudioGraph.cpp
  src/AudioFullDAGraph.cpp
  src/PluginGraphNode.cpp
  src/BuiltInGraphNode.cpp
  src/AudioGraphRegistry.cpp
  src/builtin/GainNode.cpp
  src/builtin/ChannelMergerNode.cpp
  src/builtin/ChannelSplitterNode.cpp
```

As with other modules in this repository:

- `uapmd-graph/uapmd-graph.hpp` is the stable top header;
- `detail/*` headers are not stable API.

## Proposed Core Runtime API

### `AudioGraphNode`

Generic runtime node interface used by all graph implementations.

Expected responsibilities:

- expose stable graph-local identity via `nodeId()`
- expose semantic type such as `webaudio:GainNode` or `plugin:vst3`
- expose display name
- support bypass control
- process audio
- report latency and tail
- expose bus configuration
- expose parameter update events

Plugin-backed nodes and built-in nodes both implement this interface.

### `AudioGraphPluginNode`

Optional plugin-specific interface for nodes backed by `AudioPluginInstanceAPI`.

Expected responsibilities:

- expose `instanceId()`
- expose `pluginInstance()`
- accept incoming scheduled UMP events
- support note-off / stop-flush behavior used by the current plugin path

Built-in nodes do not implement this interface.

### `AudioGraph`

Generic graph runtime interface.

Expected responsibilities:

- own generic `AudioGraphNode` objects
- provide node lookup by `nodeId`
- provide plugin-node lookup by `instanceId`
- support simple linear append/remove operations
- process track audio
- expose output bus count, latency, and tail queries
- allow engine-provided callbacks for plugin event routing

### `AudioFullDAGraph`

Generic advanced graph topology interface extending `AudioGraph`.

Expected responsibilities:

- expose full connection list
- connect/disconnect endpoints
- clear connections
- support graph input/output pseudo-nodes

## Proposed Descriptor API

`uapmd-graph` should own schema-neutral C++ graph description types used by both runtime and serialization code.

Expected descriptor families:

- `AudioGraphDescriptor`
- `AudioGraphNodeDescriptor`
- `AudioGraphConnectionDescriptor`
- `AudioGraphEndpointDescriptor`
- scalar parameter / option value types

These descriptor types are intended to:

- remove plugin-index-based addressing;
- use stable `node_id` strings;
- represent plugin nodes and Web Audio-inspired built-in nodes in one structure;
- allow `uapmd-data` to serialize graphs without owning DSP semantics.

## Built-In Node Type Policy

Built-in node types should follow public semantics rather than UAPMD-specific naming.

Examples:

- `webaudio:GainNode`
- `webaudio:ChannelMergerNode`
- `webaudio:ChannelSplitterNode`

Initial implementation target:

- `webaudio:GainNode`

Later candidates:

- `webaudio:ChannelMergerNode`
- `webaudio:ChannelSplitterNode`

## Gain Node Expectations

Initial `webaudio:GainNode` behavior:

- linear gain parameter
- recommended supported range in UAPMD: `0.0 .. 8.0`
- default value: `1.0`
- host UI may present a dB-oriented slider
- DSP should apply smoothing to avoid zipper noise
- zero latency
- zero tail
- event path is pass-through / no-op for MIDI-CI-specific behavior

## Engine Integration Policy

`uapmd-engine` should keep ownership of:

- plugin instance lifetime
- `instanceId` allocation
- default track graph policy
- MIDI-CI function block mappings
- plugin output dispatch

`uapmd-graph` should not own:

- plugin host catalog resolution
- project file IO
- function block manager policy

## Migration Strategy

Everything is done. They are left for history.

### Stage 1: Create Module

- add `source/uapmd-graph`
- add top header and `detail/` headers
- add static library target and wire it into the root build

### Stage 2: Move Existing Graph Code

- move current graph headers and implementations from `uapmd/detail/node-graph/*` into `uapmd-graph`
- keep behavior unchanged as much as possible
- update includes and link dependencies

### Stage 3: Generalize Runtime Node Model

- replace plugin-only runtime node assumptions with generic `AudioGraphNode`
- keep plugin-backed node implementation as one concrete node type
- preserve current engine behavior for plugin-only graphs
- **complete**:
  - `AudioGraphNode` and `AudioGraph` exist in `uapmd-graph`
  - `AudioPluginNode` derives from `AudioGraphNode`
  - `AudioPluginGraph` and `AudioPluginFullDAGraph` both derive from `AudioGraph`
  - both graph implementations expose generic node lookup by `nodeId`
  - both graph implementations process built-in nodes at the end of the audio chain

### Stage 4: Add Descriptor And Registry APIs

- add descriptor types under `uapmd-graph`
- add built-in node registry and factory interfaces
- **complete**:
  - graph descriptor structs exist in `uapmd-graph`
  - built-in node type constants exist under `builtin::kGainNodeType`
  - `AudioGraphRegistry` with `createDefault()` registers all built-in factories
  - both `AudioPluginGraph` and `AudioPluginFullDAGraph` use the registry to instantiate built-in nodes via `appendBuiltInNodeSimple`

### Stage 5: Implement `webaudio:GainNode`

- add runtime built-in gain node implementation
- support parameter update path and smoothed DSP
- **complete**:
  - `webaudio:GainNode` runtime node (`builtin::GainNode`) exists in `uapmd-graph`
  - default registry registers the factory; node is created via `AudioGraphNodeDescriptor`
  - applies per-buffer linear gain ramp (`current_gain_` → `target_gain_`) to avoid zipper noise
  - passes MIDI events through unchanged
  - `SequencerTrack::create` appends a terminal gain node (node id `builtin:track_gain`) to every new track's graph regardless of graph type

### Stage 6: Update Engine Integration

- let tracks host both plugin-backed nodes and built-in nodes
- keep plugin instance bookkeeping separate from generic graph node identity
- maintain plugin routing via plugin-node lookup only
- **complete**:
  - `AudioPluginGraph` stores generic `AudioGraphNode` objects; plugin nodes are inserted before built-in nodes; built-in nodes process last in the serial chain
  - `AudioPluginFullDAGraph` stores built-in nodes in a dedicated `builtin_nodes` list separate from the DAG plugin nodes; they are applied in order after DAG output routing via `advanceToNextNode()` + `processAudio()`, matching serial-graph semantics
  - migration between graph types (`AudioPluginGraph::migrate`) correctly transfers both plugin nodes and built-in nodes; `adoptNodesFromMigration` routes each to the appropriate list
  - `SequencerTrack::trackGain()` / `trackGain(double)` read and write the gain node found by `nodeId` in whichever graph is currently active
  - `SequencerEngine::processAudio` routes the final mix through the master track's graph unconditionally so the master `GainNode` always controls the output volume
  - uapmd-app exposes a per-track volume slider (non-linear dB taper, 0 dB at 70% position) and a master track volume slider, both wired to `SequencerTrack::trackGain`

### Stage 7: Update Serialization Layer

- migrate `uapmd-data` graph serialization toward descriptor-based generic nodes and `node_id`-based connections
- retain compatibility handling for legacy graph formats as needed

