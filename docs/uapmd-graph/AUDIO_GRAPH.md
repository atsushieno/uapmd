
# Audio Graph abstraction

`AudioPluginGraph` is designed to customizable enough to allow external implementation while being decoupled from other 
UAPMD components.

Currently there are two graph implementations:

- simple linear graph
- full DAG

`AudioPluginGraph` implementation classes must provide realtime-safe `processAudio()` implementation that might be in 
an isolated process such as Web `AudioWorklet` or AUv3 audio processor.

uapmd-graph also features `AudioGraph` and `AudioGraphNode` interfaces, that provide implementation-agnostic ways to process custom audio nodes such as gain or splitter, so that basic audio track operation can be performed.
The graph node API also involves saving and loading to the UAPMD project data.

Since UAPMD aims to provide product-agnostic ways to save and load project data, we try our best to avoid defining "standard set" of built-in audio nodes by ourselves.
What we do here instead is to reuse Web Audio API - we define operational graph nodes such as `webaudio:Gain`.
Nothing is saved without splitting namespaces in the serialized JSON object - we assign pseudo namespace prefixes for the factory built-in nodes.

## AudioGraphProvider

While `AudioPluginGraph` abstracts away how it organizes its plugin connections, there are handful of areas that the audio graph structure matters:

- Serialization in the project tracks: in uapmd-data, each `TimelineTrack` should contain data to populate an `AudioPluginGraph`, and it has to be serialized to and from data bytes and files.
  - it is still independent of the project serialization itself; the caller can save it in either a consolidated file or embed within the project.
- Latency compensation
- Tail length

They have to be provided per graph (which need implementation).

