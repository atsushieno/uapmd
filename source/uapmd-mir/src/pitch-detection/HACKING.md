# Hacking: pitch detection

MPM (McLeod Pitch Method) and YIN come from
[sevagh/pitch-detection](https://github.com/sevagh/pitch-detection), MIT,
Copyright (c) 2018 Sevag Hanssian.

**No upstream source is committed here.** `CPMAddPackage(... DOWNLOAD_ONLY YES)`
in `../../CMakeLists.txt` fetches the tree at a pinned commit, and the build
compiles exactly two files out of it — `src/mpm.cpp` and `src/yin.cpp` —
**unmodified**. `NOTICE` carries the MIT text, which still has to travel with
binaries even though the source does not travel with the repository.

## Why DOWNLOAD_ONLY rather than a normal dependency

Upstream's own CMakeLists cannot be used:

- Its public header includes `<mlpack/core.hpp>` and stores an
  `mlpack::hmm::HMM` inside the base class that *every* estimator derives from,
  so mlpack — and Armadillo, and a BLAS — would be mandatory even for the plain
  estimators. Those namespaces were removed in mlpack 4, so only mlpack 3 works.
- It requires FFTS, which has no package this project can consume.
- It sets `-fext-numeric-literals -fopenmp -march=native -ansi` globally, which
  neither configures under AppleClang nor survives a cross-compile.

None of that affects the algorithms themselves, only how they are packaged.

## The files here

These are substitutions, not copies. Each replaces something upstream that
cannot be built in this project; together they are what lets `mpm.cpp` and
`yin.cpp` compile untouched.

| file | what it replaces |
|---|---|
| `pitch_detection.h` | Upstream's public header. Same classes and signatures, but the mlpack HMM becomes an opaque placeholder and the FFTS plan handles are gone. Our include directory precedes upstream's `include/`, so this is the header their sources see. It also supplies `ssize_t` on MSVC, which upstream uses for loop bounds and Windows does not define -- since the sources are compiled unmodified, this header is the only place such a fix can go. |
| `autocorrelation.cpp` | Upstream's, which calls FFTS directly. Rewritten against pocketfft, which takes arbitrary transform lengths, so upstream's power-of-two versus complex-to-complex split disappears. |
| `parabolic_interpolation.cpp` | Upstream's, which reads one element past the end of the array when interpolating around its last index — reachable from YIN's `absolute_threshold()`. |
| `HmmStub.cpp` | `util::pitch_from_hmm()`, which upstream implements with mlpack. Returns -1. |

## Consequences

`pyin()` and `pmpm()` — the probabilistic estimators — are compiled but
non-functional, because the HMM they depend on is the whole reason mlpack was
required. They always return -1, which callers already treat as "no pitch".
Nothing in this project calls them.

Note segmentation lives in `../PitchTranscription.{hpp,cpp}`; these estimators
only report one fundamental per window.
