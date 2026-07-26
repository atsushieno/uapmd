# API Design Policy

NOTE: we are still not at the stage where API stability matters.

## API compatibility

- We will follow semantic versioning.
  - We will keep making breaking changes until version 1.0 release.
  - At some stage we will start maintaining API stability on every minor release (during x in version 0.x).
- We might add preprocessing symbols like `REMIDY_API` at some stage.

## ABI compatibility

- Not sure how much we consider as C++ is not for maintaining ABI compatibility, but we may start adding C API for
  ABI compatibility and cross-language usability.

## Choice of languages

- We won't switch to Rust at least until there is Tier-1 support for Android and [support in NDK](https://github.com/android/ndk/issues/1742).
- No macOS-specific code there is not likely Swift adoption as its interop is still complicating.
  - We use Objective-C++ for now, but might switch to C++ using `choc::objc` for better coding experience (namely on CLion).

## Include path stability

There are frontend header files such as `remidy/remidy.hpp` and `uapmd/uapmd.hpp`, and only those file paths are stable.
Path to an individual include file will not be stable.

## Third party library types

- No third party library types are part of stable API.
- Any third party library types exposed in the public API are not guaranteed as safe to use.
- For C++ Standard API:
  - We use `std::filesystem::path` for expressing file paths.
    - That does not necessarily mean we use it internally; they might be translated to third-party
      libraries in the implementation details.

## Modular design

UAPMD contains full of AI generated code, and they tend to be overcomplicated due to its too durable thinking brain up to the level where human beings can give up thinking through.
We, human beings, should weigh in and bring in appropriate separation of concern, so that we can conveniently reduce token consumption in our own brain.
Therefore, whenever workable, UAPMD tries to modularize the feature set into a class, a source file, or a module.

For example, our `SequencerEngine` can be quite complicated if it does not separate `LatencyCompensationManager`, `TailProcessManager`, or `FrozenTrackManager`.
It is often difficult to actually keep them separate (because [EXTENSIBILITY](../uapmd-app/EXTENSIBILITY.md) also matters).

They were split after AI generated a lot of code. It is more like a [bonsai](https://en.wikipedia.org/wiki/Bonsai) work - we adjust things as it grows, not necessarily in prior to the actual implementation.
We admit that we cannot come up with ideas before actually seeing working implementation.
