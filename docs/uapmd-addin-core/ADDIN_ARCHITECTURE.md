# UAPMD addin architecture (AI slop)

An *addin* is a user-selectable UAPMD extension. The term distinguishes this architecture from hosted audio plugins.

## Design

`uapmd-addin-core` provides the generic addin runtime: entry discovery, extension-point registration, lifecycle, enablement configuration, and native library loading. It does not depend on the engine or an application.

An addin contributes to one versioned **addin path**, such as `/uapmd/audio-graph/node-class/v1`. The path identifies an extension-point contract; it is not a library name or package identifier. Any subsystem may register an extension point with the addin manager. Subsystems retain ownership of their point implementations; they are not generically removed by the manager.

An addin package contains one or more addins. Package IDs are absolute, slash-delimited paths: they start with `/`, contain no empty segments, and do not end with `/`. UAPMD-owned packages begin with `/uapmd`, for example `/uapmd/ara`. Each addin has an ID unique within its package.

The C++ v1/v2 API is source-compatible only. `Addin`, `AddinEntry`, and `AddinHost` are defined in `uapmd-addin-core/uapmd-addin-core.hpp`. `AddinHost::extensionPoint()` returns the interface registered for a path, or `nullptr`. Extension-point APIs define their own type, ownership, threading, and realtime contracts.

External addin libraries export one `uapmd_addin_entry` symbol returning an `AddinEntry`. Built-in addins register an equivalent entry with the manager. The manager initializes only after the host has registered its extension points; it queries built-in entries first, then external libraries.

## Lifecycle and trust model

An enabled addin receives `initialize(AddinHost&)`; disabling it or shutting down calls `cleanup(AddinHost&)`. Addins must remove every callback, task, UI object, and host registration they created during cleanup. The manager does not provide a generic integrity check. A faulty addin may crash UAPMD during cleanup or external-library unload; this is accepted in the current trusted native-code model.

Addins must not throw from lifecycle callbacks. No addin callback is audio-thread safe unless its specific extension-point API explicitly provides a non-blocking realtime contract.

Sandboxing is not part of the current architecture. A future non-audio-process sandbox remains possible.

## Current behavior and limitations

On native dynamic-loading targets, the manager scans these directories at initialization:

- the local application-data `uapmd/addins` directory;
- `addins` under the application's installation prefix.

The Addin Manager displays the resolved locations. It has no arbitrary-path loader, library-name requirement, rescan action, or reload action. Missing native dependencies are handled by the platform loader before UAPMD can query the entry. No sidecar manifest or package-level dependency metadata is required.

Enablement is persisted by package ID and addin ID. Project data owned by an addin should preserve opaque data when its required addin is unavailable or disabled, and restore it when the addin becomes available again.

WebAssembly uses built-in addins only. The manager has no separate code loading or unloading there. Its current enable/disable controls invoke the built-in lifecycle immediately; a restart-only policy is not yet implemented.

Stem separation for audio import is a built-in addin, always present. ARA support is a built-in addin when `UAPMD_ENABLE_ARA` is enabled. It uses the engine's plugin-instance lifecycle extension point. `UAPMD_HAS_ARA` remains the build-time availability and license-compliance gate. ARA is disabled by default for WebAssembly because the current ARA SDK rejects `wasm32`: it has no packing/alignment definition for that architecture.

## Extension points

`/uapmd/engine/v1` exposes the `uapmd::SequencerEngine` an addin runs against.

`/uapmd/audio-graph/provider/v1` exposes the `uapmd::AudioGraphProviderRegistry`.
Providers are *contributed*, not replaced: an addin adds its own audio graph
implementation alongside the built-in simple linear and full DAG providers, and
must `remove()` whatever it added during `cleanup()`.

A provider writes a graph type id into project data, but that id does not decide
who loads the graph back. **Loadability is decided by asking providers to load
it.** A graph written by one provider is often loadable by another -- a linear
chain is a degenerate DAG -- and the provider that wrote it may not exist in the
build that opens it. The recorded type is tried first; if it is absent or its
loader fails, the remaining providers are tried in turn, and the one that
succeeds is recorded as the graph's type from then on.

This puts one requirement on providers:

- **A loader must fail when it cannot represent its input.** Accepting a graph
  and quietly dropping what it cannot express would make it the answer to every
  query and destroy the data on the next save. The built-in simple linear
  provider refuses a graph carrying explicit connections for exactly this
  reason.

When no provider can load a graph, the track runs a substitute graph and the
original graph file is carried through the next save byte for byte, under its
original graph type -- so a build that does have the provider still opens the
project intact. Replacing the track's graph type discards it, since that is the
user choosing a different graph.

The graph type id remains an exact identity elsewhere: creating a graph of a
requested type, undo snapshots, and recording what a saved graph is. Only
loading is decided by result.

Graphs offer capabilities through `uapmd_graph::AudioGraphExtension`, retrieved
with `AudioGraph::getExtension<T>()`. Edge editing lives behind
`GraphConnectionExtension`; nothing outside a provider names a concrete graph
class.

`/uapmd/audio-import/stem-separator/v1` exposes the
`uapmd::import::StemSeparatorRegistry`. Like graph providers, stem separation
backends are *contributed*: an addin adds a `uapmd::import::StemSeparator`
alongside whatever else is registered, and must `remove()` it during
`cleanup()`. Nothing in `uapmd-data` separates stems on its own, so audio
import is unavailable -- and the application hides it -- when no addin
contributed a separator. The Demucs backend lives in `uapmd-mir`, alongside
the analysis addins but built separately from them; it is a built-in addin, so
it is always present and can only be turned off at runtime.

A separation run takes minutes and happens on a worker thread, while the addin
that owns the separator can be disabled at any moment from the Addin Manager.
The registry therefore hands out `StemSeparatorRegistry::Lease` rather than a
bare pointer:

- a run holds a lease for its whole duration, and `remove()` blocks until every
  lease is released, so the code behind the separator is never unloaded while
  it is executing;
- `Lease::withdrawn()` turns true the moment `remove()` starts, and a run is
  required to poll it through its cancellation callback -- otherwise the wait
  in `remove()` would last as long as the whole separation.

Everything else about a separator (its name, its model file spec, the list of
registered separators) is read on the thread that also drives addin
enablement, and needs no lease.

## Roadmap

### v1: source-integrated addins

Addins may live in other repositories but are incorporated into the UAPMD build, for example through CPM.cmake. There is no ABI promise. The goal is to dogfood registration, initialization, cleanup, enablement, and unloading.

### v2: closely coordinated external addins

Authors may build and distribute native addins themselves, using compatible compiler toolchains and build environments. ABI compatibility is still not guaranteed. Native dependencies remain the package author's and platform loader's responsibility.

### v3: ABI-compatible addins

UAPMD will define a constrained, versioned ABI suitable for independently distributed binary addins. The existing C++ interfaces may continue evolving separately.

Future work also includes dependency metadata and diagnostics, project-data contracts for specific extension points, optional sandboxing, and carefully specified realtime extension points.
