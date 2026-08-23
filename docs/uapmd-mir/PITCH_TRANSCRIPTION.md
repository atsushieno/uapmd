# Pitch transcription: audio clip to MIDI 2.0 note clip (AI slop)

Turns an audio clip into a MIDI 2.0 note clip at the same position on the same
track, so that the notes can then be edited, re-voiced, or given automation in
the piano roll. Monophonic: a chord yields whichever partial the estimator locks
onto, or nothing at all. For polyphonic material see
[Basic Pitch](BASIC_PITCH.md), which shares this feature's note representation
and clip writer.

| | |
|---|---|
| package / addin id | `/uapmd/mir` / `pitch-transcription` |
| built when | always, wherever the `uapmd-mir` addin library is built |
| estimators | MPM and YIN, CPM-fetched; substitutions in `src/pitch-detection/` |
| note segmentation | `src/PitchTranscription.{hpp,cpp}`, namespace `uapmd_pitch` |
| addin | `src/PitchTranscriptionAddin.cpp` |

It ships in the `uapmd-mir` addin library and shares that library's package id,
but it is **not** behind `UAPMD_ENABLE_MIR`. That option gates only the
music-analysis addins in the same library, because their results are not yet
trustworthy — which does not apply here — and the licences involved are MIT (the
estimators) and BSD-3 (pocketfft), so no Apache-2.0 compliance question
arises either. Nothing here depends on librosa.cpp or libsonare, and neither is
fetched when the analysis addins are off.

`src/AddinEntry.cpp` is what makes that work: the library's exported
`uapmd_addin_entry` lives there rather than in any one addin's source, and it
assembles whichever addins were compiled in. Pitch transcription is always one
of them; the analysis addins are added only under
`UAPMD_ENABLE_MIR_ANALYSIS`.

Because the library is a dynamically loaded addin, it is built only where the
addin manager can load one. WebAssembly, Android and iOS use built-in addins
only, so transcription is currently desktop-only there.

## Entry points

| | one clip | whole project |
|---|---|---|
| extension point | `/uapmd/app/clip-command/v1` | `/uapmd/app/command/v1` |
| id | `uapmd-pitch.transcribe-audio-clip` | `uapmd-pitch.transcribe-all-audio-clips` |
| title | "Transcribe to mono MIDI2 clip (pitch-detection)" | "Transcribe all audio clips to MIDI2 clip for each" |
| order | 100 | 1100 |
| offered in | the clip context menu, on audio clips only | the Command menu |

Both drive the same worker thread, so neither can start while the other is
running, and both report progress by changing their own menu title while they
work.

## Estimators

MPM (McLeod Pitch Method) and YIN come from
[sevagh/pitch-detection](https://github.com/sevagh/pitch-detection), fetched by
CPM with `DOWNLOAD_ONLY` and compiled unmodified against a replacement public
header; upstream's own CMakeLists cannot be used, because its header drags in
mlpack 3 and FFTS even for the plain estimators. No upstream source is
committed. `src/pitch-detection/HACKING.md` records what is substituted and why.
MPM is the default: it is steadier on sustained tones, where YIN follows a
moving pitch faster but jitters more.

Each returns one fundamental for one window of samples, and nothing else. There
is no voicing probability, no onset detection, and no polyphony -- everything
that turns those per-window frequencies into notes is
`PitchTranscription`.

## From frequencies to notes

1. **Frame.** The signal is cut into `frame_length` (2048) windows advancing by
   `hop_length` (512).
2. **Gate on level.** A frame quieter than `silence_threshold_db` (-50 dB RMS)
   is unvoiced without running the estimator at all.
3. **Estimate.** The remaining frames go through MPM or YIN. Results outside
   `fmin`..`fmax` (A1..C7) are discarded as octave errors or rumble, and the
   rest become fractional MIDI note numbers.
4. **Median-filter.** Each frame takes the median of the voiced pitches within
   `median_filter_frames` (5) of it. A single frame that jumped an octave is
   outvoted; a frame whose neighbourhood is mostly unvoiced is dropped, which
   trims the ragged edges of a note instead of letting them start it early.
5. **Segment.** A note continues while consecutive voiced frames stay within
   `pitch_tolerance_semitones` (0.8) of the note's running median -- compared
   against the median rather than the previous frame, so a slow glide stays one
   note while a step change starts another. Silence, or a step outside the
   tolerance, ends it. Anything shorter than
   `min_note_duration_seconds` (0.06) is discarded as estimator noise.

A frame's estimate is dated to the centre of its window, not its leading edge,
and note boundaries fall half a hop outside the first and last centres.
Timestamping by the leading edge instead drags every onset earlier by up to half
a window.

Velocity comes from the loudest frame in the note, with `silence_threshold_db`
as the floor of the range.

## What the clip contains

One MIDI 2.0 Note On/Note Off pair per note, on group 0 / channel 0, sorted by
tick. Ticks come from the project tick resolution and tempo (480 PPQ and 120 BPM
when the project has not established either).

Each note carries its measured pitch as a **Pitch 7.9 note attribute**
(attribute type 3) alongside the note number. The note number is the nearest
semitone; the attribute is what was actually played, to the cent. A performance
that sat 14 cents flat keeps those 14 cents rather than having them rounded away
at transcription time.

The new clip takes the audio clip's position and its full duration, rather than
ending at its own last note -- a clip that stopped at the last note would leave
nowhere to draw automation over the tail of the audio it came from.

## Accuracy

On a synthesized C4-E4-G4-C5 line with three harmonics, half-second notes and
100 ms gaps, both estimators recover all four note numbers exactly. YIN places
onsets within ~1 ms and MPM within ~25 ms; MPM is the earlier of the two because
its periodicity test accepts a window that the note has only partly filled.
Measured pitch runs a few cents flat of the true value (~14 cents at C4, ~3 at
C5), which is ordinary for autocorrelation-based estimation over a short window,
and which the Pitch 7.9 attribute preserves rather than hides.

Real recordings are harder than a synthetic tone in every way this pipeline is
sensitive to: percussive attacks, vibrato wider than the segmentation tolerance,
and any polyphony at all.
