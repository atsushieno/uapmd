# Basic Pitch: polyphonic transcription (AI slop)

Turns an audio clip into a **polyphonic** MIDI 2.0 note clip at the same
position on the same track, with per-note pitch bend. Where the
[monophonic estimators](PITCH_TRANSCRIPTION.md) return one fundamental per
window — and answer a chord with a phantom note an octave below, or with
nothing at all — this recovers the individual notes of a chord.

| | |
|---|---|
| package / addin id | `/uapmd/basic-pitch` / `transcription` |
| CMake target | `uapmd-basic-pitch` (aliased `uapmd::basic-pitch`) |
| built when | `UAPMD_ENABLE_BASIC_PITCH`, **off by default** |
| model | [spotify/basic-pitch](https://github.com/spotify/basic-pitch) `nmp.onnx`, Apache-2.0 |
| sources | `src/basic-pitch/` |

## Why it is opt-in

The option gates a **download**, not just a build. The model is Apache-2.0,
including its weights, and they are not committed here, so an enabled build
fetches `nmp.onnx` at configure time and embeds it into the binary. That makes
enabling it a redistribution decision, which is why it defaults off rather than
following the opt-out shape `UAPMD_ENABLE_LIBSONARE` uses.

The weights are not the only Apache-2.0 material involved.
`BasicPitchTranscriber.{hpp,cpp}` is a port of upstream's `note_creation.py`,
which makes it a derivative work, and unlike the weights it *is* committed — so
it carries Apache-2.0 regardless of the option. That is consistent with the
top-level README, which already releases the whole `uapmd-mir` module under
Apache V2, and it was a deliberate choice: the note decoder's thresholds were
tuned against this model, and departing from upstream would have cost notes and
forfeited the ability to validate against upstream's own decoder.
`src/basic-pitch/HACKING.md` records the provenance of every file.

The download is pinned by SHA-256 rather than by tag: `main` is a moving branch,
and a silent upstream retrain must fail the build rather than quietly change
what ships. The tensor names in `BasicPitchModel.cpp` are safe for the same
reason — they cannot drift without the download failing first.

Embedding, rather than shipping a data file beside the app, is what lets this
work on WebAssembly and Android, where there is nowhere sensible to read one
from. The download becomes `basic_pitch_model_data.cpp` in the build directory —
generated, never committed — carrying the 230,444 bytes of `nmp.onnx` verbatim
as hex text in 60,000-character chunks, because MSVC caps a string literal at
65535 bytes.

That generated file is the largest piece of Apache-2.0 material in the build,
and it is what the compiler and the shipped binary actually see, so it carries
the download URL, the pinned hash and the Spotify copyright in its own header.
Any binary built with this option redistributes the weights, so
`src/basic-pitch/LICENSE` and `src/basic-pitch/NOTICE` have to accompany it.
`src/basic-pitch/HACKING.md` records the full provenance of every file,
generated ones included.

It is a **built-in** addin — a static library the application links
`WHOLE_ARCHIVE` — with no platform guard, so it reaches every target including
the ones where dynamic addin loading is unavailable.

## Entry points

| | one clip | whole project |
|---|---|---|
| extension point | `/uapmd/app/clip-command/v1` | `/uapmd/app/command/v1` |
| id | `uapmd-basic-pitch.transcribe-audio-clip` | `uapmd-basic-pitch.transcribe-all-audio-clips` |
| title | "Transcribe to poly MIDI2 clip (basic-pitch)" | "Transcribe all audio clips to poly MIDI2 clip (basic-pitch)" |
| order | 101 | 1200 |

Both drive one worker thread, so neither can start while the other runs, and
both report progress by changing their own menu title. They sit directly beside
the monophonic commands in the same menus.

## The model is a weight file, not a graph

`nmp.onnx` holds 248 nodes, but roughly three quarters of them are `Reshape`,
`Unsqueeze`, `Transpose` and `Pad` — shape plumbing that is entirely static
because the input length is fixed at 43,844 samples. Only ~30 convolutions and
a dozen elementwise operations compute anything, and the whole file is 230 KB
with 140 KB of that being weights.

So the network is written out directly in `BasicPitchModel.cpp` and the ONNX
file is read only for its tensors. `OnnxWeights.cpp` is a bounds-checked
protobuf reader for four fields — `graph`, `initializer`, `dims`, `raw_data` —
which is why neither libprotobuf nor onnxruntime is a dependency; neither could
be carried to WebAssembly and Android without a fight.

## Stages

1. **CQT** (`cq_t2010v2`). Nine octaves. Each one reflect-pads the signal by
   128 samples, applies two banks of 36 kernels of 256 taps at a hop that halves
   per octave (256 down to 1), then decimates the signal by two through a
   256-tap lowpass. All nine octaves share one kernel pair and one lowpass —
   that reuse is what makes the transform cheap. Bins are concatenated from the
   lowest octave upward and the top 309 of 324 are kept.

   The exported graph negates one of the two banks and swaps which one, per
   octave. Neither matters: only `sqrt(a² + b²)` is used, so the sign and the
   ordering fall out of the arithmetic and are simply not implemented.

2. **Normalised log.** `10·log10(magnitude² + 1e-10)`, min-max scaled to [0, 1]
   over the whole window, with the degenerate all-equal case pinned to zero.
   Then the BatchNormalization that followed it, folded by the exporter to
   `×2.480741 − 0.87691832`.

3. **Harmonic stacking.** Eight copies of the spectrum shifted by
   `round(36·log2(h))` bins for `h` in `[0.5, 1, 2, 3, 4, 5, 6, 7]`, so that the
   harmonics of a pitch line up on that pitch's bin and a small 2-D kernel can
   see a whole harmonic series at once.

4. **Six convolutions** producing three posteriorgrams: `contour` (264 bins, 3
   per semitone), `note` and `onset` (88 pitches each). The onset head is fed
   the note head's output alongside its own stem — an onset is a note starting,
   so the two are deliberately not independent.

## Windowing and note creation

The network only ever sees two seconds. A clip is resampled to 22050 Hz mono,
padded by half the overlap, and fed through as windows advancing by 36,164
samples; 15 frames are dropped from each end of every window's 172 before the
remainder are stitched, so a note near a boundary is decided by the window that
saw the most context around it.

`BasicPitchTranscriber.cpp` then ports upstream's `note_creation.py` faithfully,
because its thresholds were tuned against this model:

- onsets are strict local maxima above 0.5, walked **backwards** in time so that
  when two onsets compete for the same energy the later one is not truncated;
- frame energy above 0.3 keeps a note on, ending it after 11 consecutive frames
  below;
- **inferred onsets** add peaks wherever frame energy jumps, catching legato
  notes the onset head misses;
- the **melodia pass** harvests whatever energy no onset explained, growing each
  remaining peak in both directions;
- notes shorter than 127.7 ms are dropped;
- pitch bend per note comes from the contour head, Gaussian-weighted within 25
  bins of the expected one, in thirds of a semitone.

Resampling uses a windowed-sinc kernel cut off at whichever Nyquist is lower.
Plain interpolation would fold everything above 11 kHz back into the band, and a
pitch model reads those images as real partials.

## Accuracy and cost

Checked against onnxruntime running the original graph on the same input. Every
stage agrees to within float32 round-off:

| stage | rel RMS |
|---|---|
| cqt_magnitude | 1.9e-07 |
| normalized_log | 1.2e-04 |
| harmonic_stack | 2.1e-04 |
| contour | 8.6e-05 |
| note | 4.7e-05 |
| onset | 1.4e-04 |

The error shrinks through the network: convolution followed by sigmoid damps the
front end's log-amplified round-off rather than amplifying it. Note decoding
was checked separately against upstream's own `output_to_notes_polyphonic` on
identical posteriorgrams, and matches exactly — same frames, pitches,
amplitudes and bend values, including the melodia pass. End to end over a
six-second, four-window signal the two pipelines produce identical note lists.

The convolutions are plain loops, deliberately: they are validated against
reference activations, so they are written to be obviously correct. About 0.5
GMAC per two-second window works out at roughly 0.4x realtime, so a three-minute
clip takes about a minute. If that becomes the bottleneck, the convolutions are
the place to look, and ggml is already in the tree.
