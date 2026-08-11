# UAPMD addin architecture (AI slop)

UAPMD is evolving towards an addin-based architecture.  An addin is a
user-selectable implementation of a UAPMD extension point.  We use *addin*
instead of *plugin* here to distinguish this architecture from hosted audio
plugins.

This document specifies the foundation: discovery, registration, lifecycle,
configuration, and package identity.  It deliberately does not yet specify
the interface types for individual extension points.  Those interfaces are to
be designed when each extension point is introduced.

## Goals

- Users can enable and disable individual addins in an Addin Manager.
- On platforms that support it, addin libraries can be loaded and unloaded at
  runtime.  This is a development and correctness goal: it continuously tests
  that addins cleanly disconnect from the host.
- An addin is responsible for safely unplugging itself.  An addin that leaves
  callbacks, UI objects, tasks, or other host references behind during cleanup
  is defective and may crash UAPMD when it is unloaded.
- Addin support is introduced at selected UAPMD layers.  It is not a general
  extension mechanism for every internal API.
- WebAssembly retains the Addin Manager and per-addin user configuration, but
  uses compiled-in addins for now.  Enabling or disabling an addin takes effect
  on restart there; its WebAssembly code is not loaded or unloaded separately.

Sandboxing is not a requirement for the initial phases.  Addins are trusted
native code.  A later, non-audio-process sandbox model remains possible but
is outside this design.

## Concepts

### Addin path

An **addin path** identifies and versions an extension point, for example
`/uapmd/audio-graph/node-class/v1`.  It tells the host where a contribution is
accepted and which interface contract applies.  It is not a library name,
package identifier, or dependency declaration.

### Addin

An **addin** is one contribution to one addin path.  One addin can be enabled
or disabled independently.  Its concrete interface is defined by its addin
path.

### Addin package

An **addin package** is a library and the addins it exports.  A package has a
stable package ID.  It may expose related contributions, such as an audio
graph node class and its corresponding project serialization support.

Package IDs are used to record configuration and project dependencies.  An
addin contribution has its own ID within its package.

## Extension points

Candidate extension points include:

- project data readers and writers, including additional project data;
- audio graph nodes;
- audio graph implementations;
- playback engine hooks;
- additional hosted audio-plugin formats;
- clip editor types such as piano roll, MIDI dump list, step sequencer, score,
  and MML editors.

The first concrete extension points should be selected for safety and clarity.
The addin framework is implemented before these interface contracts are
committed to.

## Addin entry and lifecycle

Each addin library exports one entry containing its addin list.  The concrete
v1 C++ API, entry symbol, ownership rules, and loader call flow are defined in
the **Public API and implementation plan** section below.

The host performs initialization in a deterministic order and cleanup in the
reverse of that order.  It must not invoke cleanup on the audio thread.
Libraries that support dynamic unloading are unloaded only after their enabled
addins have been cleaned up.  Crashes during cleanup or unload are treated as
evidence of a broken addin lifecycle and are useful failures in this early
architecture.

## Public API and implementation plan

The framework is a reusable UAPMD concern, not UI policy.  Introduce a new
`source/uapmd-addin-core` module before adding loader code to `uapmd-app`.

### Module and build changes

- Add `source/uapmd-addin-core/CMakeLists.txt`, initially producing a static
  `uapmd-addin-core` target and `uapmd::uapmd-addin-core` alias.
- Add `source/uapmd-addin-core/include/uapmd-addin-core/uapmd-addin-core.hpp` as the stable
  top-level public header.  It includes all API declarations below.  Clients
  include only this header; files under `detail/` are not stable include paths.
- Add `uapmd-addin-core` to `source/CMakeLists.txt` before consumers, add its
  include directory to `_UAPMD_PUBLIC_INCLUDE_SUBDIRS`, and add its target to
  the installed UAPMD libraries.
- `uapmd-addin-core` depends only on the C++ standard library.  In particular, it
  must not depend on `uapmd-engine`, `uapmd-graph`, `uapmd-data`, or any app
  target.  Extension-point APIs depend on this module, not the reverse.
- Add an `add_uapmd_addin_library(target)` CMake helper.  It links the target
  to `uapmd::uapmd-addin-core`, applies the correct shared-library properties, and
  makes `uapmd_addin_entry` visible to `dlsym()`/`GetProcAddress()`.

### v1 public header

The first public header has a deliberately C++-only API.  It is source
compatible only; its layout and vtables are not ABI promises before v3.

```cpp
#pragma once

#include <span>
#include <string_view>

namespace uapmd {

class AddinHost;

struct AddinIdentity {
    std::string_view package_id;
    std::string_view addin_id;
};

class Addin {
public:
    virtual ~Addin() = default;

    virtual AddinIdentity identity() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;
    virtual std::string_view path() const noexcept = 0;

    virtual bool initialize(AddinHost& host) noexcept = 0;
    virtual void cleanup(AddinHost& host) noexcept = 0;
};

class AddinEntry {
public:
    virtual ~AddinEntry() = default;

    virtual std::string_view packageId() const noexcept = 0;
    virtual std::span<Addin* const> addins() noexcept = 0;
};

class AddinHost {
public:
    virtual ~AddinHost() = default;

    // Returns the path-specific host interface, or nullptr when unsupported.
    virtual void* extensionPoint(std::string_view path) noexcept = 0;
};

using AddinEntryFunction = AddinEntry* (*)() noexcept;

} // namespace uapmd

// This is the sole required native-library symbol in v1 and v2.
extern "C" UAPMD_ADDIN_EXPORT uapmd::AddinEntry* uapmd_addin_entry() noexcept;
```

`UAPMD_ADDIN_EXPORT` is defined by this header.  It expands to the appropriate
export declaration for Windows and default symbol visibility for platforms
that hide symbols by default.  The loader does not link against this symbol;
it obtains it by name from a loaded library and casts it to
`uapmd::AddinEntryFunction`.

`AddinIdentity` is deliberately two-part.  `package_id` identifies the
library/package that owns the addin; `addin_id` identifies one contribution in
that package.  Both and `path()` must be non-empty, stable for the lifetime of
the loaded library, and copied by the app when persisted in settings or a
project.

`Addin::initialize()` is called at most once between successful load and
cleanup.  It returns `false` after rolling back any partial registration.
`Addin::cleanup()` is called only after a successful initialization, exactly
once before unload, and must remove all registrations made by that addin.
Neither callback may throw.  A failed `initialize()` leaves the addin disabled
and the library may remain loaded for its other addins.

`AddinEntry` and every `Addin*` returned from it are owned by the addin
library.  They are borrowed references: UAPMD must not delete them, retain
them after cleanup, or dereference them after unload.  The array returned by
`addins()` must retain its address and order until the library unloads.

`AddinHost` is the only generic host interface.  `extensionPoint(path)` is a
typed-at-the-caller boundary during v1/v2: an addin requests its own addin
path and casts the returned pointer to the interface defined for that path.
The host returns `nullptr` if the path is unsupported.  The returned
path-specific interface owns registration and unregistration; the generic
framework does not invent an untyped `void*` contribution registry.

Every future extension-point interface documents which `AddinHost` path it is
obtained from, its ownership rules, and whether any method is realtime-safe.
No callback supplied by an addin is audio-thread callable unless that
extension-point API expressly says so and defines its non-blocking contract.

### Private implementation classes

The following classes are required but remain app-private.  They are not
included in `uapmd-addin-core/uapmd-addin-core.hpp` and do not become addin-author API.

| Class | Location | Concrete responsibility |
| --- | --- | --- |
| `AddinLibrary` | `source/uapmd-engine/src/addins/AddinManager.cpp` | RAII state for one native library handle, its resolved `AddinEntryFunction`, entry, and descriptors.  It is private to `AddinManager`. |
| `AddinLoader` | `source/uapmd-engine/src/addins/AddinManager.cpp` | Opens a candidate library, resolves exactly `uapmd_addin_entry`, validates non-empty/unique package and addin IDs plus paths, and creates an `AddinLibrary`.  It reports a load error without calling the entry when the platform loader fails. |
| `AddinManager` | `source/uapmd-engine/include/uapmd-engine/detail/addins/AddinManager.hpp`, `source/uapmd-engine/src/addins/AddinManager.cpp` | Owns `AddinLibrary` instances and the enable state.  It serializes enable, disable, reload, and shutdown and calls lifecycle callbacks in deterministic order. |
| `EngineAddinHost` | `source/uapmd-engine/src/addins/AddinManager.cpp` | Implements `AddinHost` for the engine.  It maps a path to an extension-point-owned host interface and returns `nullptr` for unavailable paths. |
| `AddinSettings` | `source/uapmd-engine/src/addins/AddinManager.cpp` | Persists `package_id` + `addin_id` enabled state.  It contains no native handles and is usable before libraries are loaded. |
| `AddinManagerWindow` | `source/tools/uapmd-app/gui/AddinManagerWindow.hpp/.cpp` | Displays discovered addins, enable state, path, and most recent loader/lifecycle failure.  It invokes `AddinManager`; it never calls addin lifecycle functions itself. |

`AddinManager` is the ownership boundary that makes unloading testable.  It
records `Inactive`, `Initializing`, `Active`, `CleaningUp`, and `Failed`
state per addin.  State changes run on the app/control thread only.  It stops
host dispatch through the path-specific interface before invoking `cleanup()`;
after cleanup it asks that interface to verify that no registration owned by
the addin remains.  Any remaining reference is an addin or extension-point
bug and prevents normal unload only long enough to make the failure visible.

### Loader call flow

1. `AddinManager` scans its custom addin directory for platform dynamic
   libraries, then `AddinLoader` opens each candidate.  The platform dynamic
   loader resolves the library's ordinary native dependencies at this point.
   On failure, record the loader error and do not call into the library.
2. Resolve `uapmd_addin_entry`; reject the library if the symbol is absent or
   the entry is null.
3. Call the entry once, enumerate `AddinEntry::addins()`, and validate that
   each identity and path is non-empty and unique within its package.
4. `AddinSettings` selects descriptors enabled by default/user preference.
5. `AddinManager` calls `initialize(EngineAddinHost&)` for each selected addin.
   A successful return changes its state to `Active`; `false` rolls back and
   records `Failed`.
6. Disable/reload/shutdown first stops dispatch through the addin's
   path-specific interface, then calls `cleanup(EngineAddinHost&)`, verifies that
   path-specific registrations are gone, and marks the addin inactive.
7. When every descriptor in an `AddinLibrary` is inactive, destroy its
   borrowed references before closing the native library handle.  UAPMD never
   destroys the entry or descriptors themselves; the library owns them.

This phase has no generic dependency-order or serialization API.  Native
library dependencies remain platform linker responsibilities.  Project-data
extension points later use `AddinIdentity` for unavailable-data preservation.

### Implementation sequence

1. Create the dependency-free `uapmd-addin-core` module, header, export macro, and
   `add_uapmd_addin_library()` CMake helper.
2. Implement `AddinLibrary`, `AddinLoader`, `AddinManager`, and
   `AddinSettings` without UI, with a minimal test addin in a separate CMake
   target.
3. Add `EngineAddinHost` that returns `nullptr` for all paths, then validate
   discovery, initialization, cleanup, unload, and reload independently of a
   concrete extension point.
4. Add `AddinManagerWindow` and make enable/disable/reload operations invoke
   `AddinManager` on the control thread.
5. Add the first non-realtime path-specific host interface and let
   `EngineAddinHost` expose it.  Its registration object must prove that cleanup
   removed the addin's contributions.
6. Only after this lifecycle works repeatedly, add higher-risk engine or
   audio-thread extension points with their own explicit RT contracts.

## Discovery, loading, and dependencies

For v1, UAPMD scans one custom addin directory owned by its local application
data: `uapmd/addins`.  The Addin Manager displays this exact location and tells
users to install addin libraries there.  It does not offer direct loading from
an arbitrary file path or runtime library reload.  UAPMD does not reserve a
universal `/usr` location.

The scanner considers platform dynamic-library files in that directory without
requiring a library-name convention.  Libraries without the
`uapmd_addin_entry` symbol are ignored; a candidate that fails to load is
reported as an addin load error.

Addin paths determine contribution routing after a library has been loaded;
they are not a native-library discovery mechanism.

No sidecar addin manifest is required.  Addin libraries may depend on other
native libraries, including closed-source addin libraries.  Those dependencies
are ordinary platform link dependencies and must be installed where the
platform dynamic loader can resolve them.  For example, Android libraries are
packaged under the APK's `lib/<abi>/` directory.  If a native dependency is
missing, the platform loader rejects the addin library before UAPMD calls its
entry point.

This foundation does not require UAPMD to resolve addin-package dependencies
before loading.  Package-level dependency metadata, validation, and richer
diagnostics can be added later if they become useful.

## Configuration and projects

The Addin Manager persists whether each known addin is enabled.  On supported
native targets, enabling, disabling, and reloading exercises the lifecycle and
loads or unloads the relevant library.  On WebAssembly, changes are persisted
and applied at the next restart by initializing only enabled compiled-in
addins.

Project data that is owned or interpreted by an addin records the package ID
and contribution ID that it requires.  When the relevant addin is missing or
disabled, UAPMD must preserve its opaque project data without data loss,
clearly report the unavailable requirement, and restore it if the addin later
becomes available.  The behavior of unavailable features is extension-point
specific; project loading must not silently discard their data.

## Compatibility phases

### v1: source-integrated addins

Addins may be developed in separate repositories, but UAPMD incorporates and
compiles them in its own build, for example via CPM.cmake.  There is no ABI
promise.  C++ interfaces may be used freely once individual extension points
are defined.

The priority is to establish and dogfood discovery, registration,
enable/disable configuration, initialization, cleanup, and unload behavior.

### v2: externally built native addins

Authors can build and distribute addins themselves.  ABI compatibility is
still not guaranteed: this is for developers working closely with UAPMD who
use compatible compiler toolchains and build environments.  Native library
dependencies remain the responsibility of the package and platform loader.

### v3: ABI-compatible addins

UAPMD introduces and maintains an ABI boundary for independently built binary
addins, with compatibility and versioning policies appropriate to that public
distribution model.  This is expected to use a deliberately constrained ABI;
the earlier C++ interfaces remain free to evolve separately.

## Built-in addin directions

Possible built-in addins include Web Audio nodes, latency-compensation
management, track freezing, SFZ and JSFX formats, and the clip editor types
listed above.  Whether an existing built-in feature becomes an addin is a
separate decision for each extension point.
