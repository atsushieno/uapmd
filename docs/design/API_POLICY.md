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

## Third party library types

- No third party library types are part of stable API.
- Any third party library types exposed in the public API are not guaranteed as safe to use.
- For C++ Standard API:
  - We use `std::filesystem::path` for expressing file paths.
    - That does not necessarily mean we use it internally; they might be translated to third-party
      libraries in the implementation details.
