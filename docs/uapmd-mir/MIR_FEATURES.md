# uapmd-mir: music information retrieval features (AI slop)

`uapmd-mir` is an addin package that analyzes the audio clips in the current project
and writes the result back as **master-track MIDI clips** carrying tempo,
time-signature and chord meta events. It ships two independent analysis backends
that produce the same kind of output through the same writer:

| | libsonare backend | librosa.cpp backend |
|---|---|---|
| command | `src/MirAddin.cpp` | `src/MirLibrosaAddin.cpp` |
| analysis | `src/MirTempoAnalysis.cpp` + `src/MirRhythmAnalysis.cpp` (`uapmd_mir::sonare`) | `src/MirLibrosaAnalysis.cpp` (`uapmd_mir::librosa_cpp`) |
| addin id | `/uapmd/mir` / `analysis` | `/uapmd/mir` / `librosa` |
| command id | `uapmd-mir.analyze-project` | `uapmd-mir.populate-master-librosa` |
| command title | "Populate master track (libsonare)" | "Populate master track (librosa.cpp)" |
| command order | 1000 | 1001 |
| built when | `UAPMD_ENABLE_MIR` **and** `UAPMD_ENABLE_LIBSONARE` | `UAPMD_ENABLE_MIR` |

Both addins are exposed by the single `uapmd_addin_entry` in `MirAddin.cpp`, which
returns a two-element `AddinEntry` (`MirAddin` plus the one from
`uapmd_mir_librosa_addin()`). Both register on the `/uapmd/app/command/v1`
extension point and obtain the engine from `/uapmd/engine/v1`.

The two backends are deliberately structured the same way. Everything that is not
a call into libsonare or librosa lives in shared code that both use:

| shared unit | what it owns |
|---|---|
| `src/MirTempoMap.{hpp,cpp}` | analysis-window scheduling, octave/short-run resolution, run grouping, transition refinement, `tempoMapTimeToTicks` |
| `src/MirMeterMap.{hpp,cpp}` | per-beat accent strengths, the meter-segmentation DP, the candidate meter table |
| `src/MirDiagnostics.hpp` | the buffered raw-value diagnostic log |

A backend supplies only primitives: a per-window tempo estimate
(`TempoWindowEstimate`: BPM, confidence, and scored candidates), an onset
strength envelope (`OnsetEnvelope`), a beat grid, and chord labels. Both then
feed the same writer.

This document describes what each backend actually computes, what contract the
result has to satisfy for the timeline to display it correctly, and where the two
implementations diverge. It is written because the analysis results are known to
be wrong (see [Where the timing goes wrong](#where-the-timing-goes-wrong)) and the
reason is spread across three layers.

## Build configuration

**The whole module is off by default.** The analysis results are not trustworthy
yet — see [Where the timing goes wrong](#where-the-timing-goes-wrong) — so
`source/CMakeLists.txt` declares `option(UAPMD_ENABLE_MIR ... OFF)` and only calls
`add_subdirectory(uapmd-mir)` when it is set:

```bash
cmake -B cmake-build-debug -G Ninja -DCPM_SOURCE_CACHE=~/.cache/CPM/uapmd -DUAPMD_ENABLE_MIR=ON
```

Both CPM declarations live inside `source/uapmd-mir/CMakeLists.txt`, so a build
without `UAPMD_ENABLE_MIR` neither fetches nor compiles librosa.cpp or libsonare.
The module's own CMakeLists starts with a `FATAL_ERROR` guard so it cannot be
pulled in by accident.

Within an MIR-enabled build:

- **librosa.cpp** (pinned by commit) is mandatory — it is the baseline backend, so
  the module always compiles with `UAPMD_ENABLE_LIBROSA=1`.
- **libsonare** `v1.7.2` is selected by `option(UAPMD_ENABLE_LIBSONARE ... ON)`,
  and is skipped on Windows, iOS and Emscripten regardless. When it is
  unavailable, `MirAddin.cpp` compiles down to an addin whose `initialize()`
  succeeds and registers nothing, and `MirTempoAnalysis.cpp` /
  `MirRhythmAnalysis.cpp` are not compiled at all. `UAPMD_ENABLE_LIBSONARE` has no
  effect on its own.

`uapmd-mir` is a `SHARED` library deployed through `add_uapmd_addin_library()` /
`deploy_uapmd_addin_library()`, i.e. it is loaded as an external addin, not linked
into the app. A build with the module disabled simply has no such addin to load.

## Shared pipeline

Both commands run the same eight steps on a detached `std::thread` owned by the
`Command` object. The command's `title()` doubles as a progress readout while
`running_` is set, and `enabled()` returns false during a run.

```
audio source (whole file)
  -> mono downmix
  -> backend analysis            (tempo map / beat grid / meter map / chords)
  -> AnalysisResult              (times in SECONDS, relative to the audio source)
  -> seconds -> ticks conversion (shared tempo-map integration)
  -> makeMasterClip()            (UMP stream + absolute tick timestamps)
  -> TimelineFacade::addMasterMidiClip()
  -> MidiClipSourceNode          (ticks -> samples, using the same tempo map)
  -> MasterTrackSnapshot         (samples -> seconds, plus clip start offset)
  -> uapmd::TempoMap             (seconds -> quarter-note beats)
  -> BeatsSequenceEditor         (beats -> bar/beat lines)
```

### 1. Source selection

Both walk `view.audioSourceIds()`. A source is skipped when it has no frames, no
channels or no sample rate, when its owning clip cannot be resolved, or when its
clip **overlaps any MIDI clip on any track** (`intervalsOverlap`). The intent is
"do not analyze regions that already have authored MIDI".

### 2. Reading samples

```cpp
view.readAudioSourceSamples(sourceId, 0, source->frameCount, destinations.data(), source->channelCount);
```

This reads the **entire audio file from frame 0**, in the *file's* channel count
and *file's* sample rate — `ProjectAudioSourceSnapshot::sampleRate` /
`frameCount` come straight from the file header via
`FileAudioSourceRepository::getAudioSourceInfo()`. It is not the resampled,
warped, or clip-trimmed material the timeline plays.

Channels are then averaged into a mono `std::vector<float>`. Both analysis
entry points take `std::span<const float>`; the librosa.cpp side converts to
`librosa::ArrayXr` (double) internally.

### 3. `AnalysisResult`

Each command file declares its own private copy of the same struct — they are
not shared, but they are field-for-field identical:

```cpp
struct AnalysisResult {
    uapmd::TimelinePosition position;       // clip start on the timeline
    uint32_t tick_resolution;               // TPQN
    int64_t duration_samples;               // clip duration, TIMELINE sample-rate domain
    double sample_rate;                     // FILE sample rate
    double bpm;                             // "the" tempo, used as fallback
    uint8_t time_signature_numerator/denominator;
    std::vector<std::tuple<double, uint8_t, uint8_t>> time_signatures;  // (seconds, num, den)
    std::vector<std::pair<double, double>> tempo_points;                // (seconds, bpm)
    std::vector<std::pair<double, std::string>> chords;                 // (seconds, label)
};
```

Every time inside it is **seconds from the start of the audio source**, and the
whole result is anchored to the timeline by `position` alone.

### 4. Tick resolution

libsonare searches for a usable TPQN: `timeline.state().projectTickResolution`
first, then the tick resolution of any non-MIR MIDI clip on the master track,
then on any track, then 480. librosa.cpp only checks
`timeline.state().projectTickResolution` and otherwise uses 480. In practice
`TimelineFacadeImpl::addMidiClipToTimelineTrack()` rescales incoming ticks to the
project resolution anyway (`MidiClipReader::rescaleTicks`), so the difference is
cosmetic.

### 5. Seconds → ticks

Both backends call `analysisTimeToTicks` → `uapmd_mir::tempoMapTimeToTicks`,
which integrates the piecewise-constant tempo map:

```
ticks(t) = Σ_segments (segment length in seconds) * bpm_segment / 60 * TPQN
```

`MidiClipSourceNode::computeSampleTimeline()` is the exact inverse of this, so
the seconds → ticks → samples round trip is consistent.

### 6. `makeMasterClip()`

Both build the same UMP stream: `deltaClockstamp(0)`, `dctpq(tickResolution)`,
`deltaClockstamp(0)`, `startOfClip()`, then a tick-sorted, priority-sorted event
list —

| priority | event |
|---|---|
| 0 | `UmpFactory::tempo(group 0, channel 0, 10ns-per-quarter)` |
| 1 | `UmpFactory::timeSignatureDirect(group 0, channel 0, numerator, denominator, 0)` |
| 2 | `UmpFactory::metadataText(... "MIR chord <label>")` |
| 3 | `endOfClip()` at the clip-duration tick |

each preceded by a `deltaClockstamp` delta. The `denominator` passed here is the
literal value (4, 8), which matches both the Flex Data time-signature encoding and
`uapmd::MidiTimeSignatureChange::denominator` — that convention is correct.

Note that the hand-written delta clockstamps largely do not matter. Scheduling
uses the parallel *absolute* tick array passed as `umpTickTimestamps`, and
project save re-derives the whole delta stream in
`ProjectSerialization.cpp: buildSmf2ClipFromMidiNode()`. The 20-bit clamp in
`appendDelta` (`std::min<uint64_t>(delta, 0xFFFFFu)`) therefore never reaches
disk. It is dead weight, not a live bug.

### 7. Writing to the master track

Existing master clips whose name starts with `"MIR: "` are removed, then results
are inserted in `position.samples` order, skipping any result that starts before
the previous one ended. Each insertion is a separate
`ProjectMutationOrigin::User` mutation so undo history captures clip fragments
outside a document transaction — `MirAddin.cpp` has a comment explaining this;
`MirLibrosaAddin.cpp` does the same without the comment.

Clips are added with `needsFileSave = false` and an empty filepath, so on save
`ProjectSerialization` re-exports them to a fresh `.midi2` file anyway
(`needsExport` is true when the path is empty).

## Backend A: libsonare

`MirAddin.cpp` orchestrates; `MirTempoAnalysis.cpp` and `MirRhythmAnalysis.cpp`
supply libsonare's primitives to the shared tempo/meter code, in namespace
`uapmd_mir::sonare`.

### Tempo map — `uapmd_mir::sonare::detectTempoMap()`

This is a **sliding-window re-estimation**, not a tempo tracker.

1. `buildTempoMap()` schedules windows of `kTempoWindowSeconds = 12.0` s at
   `kTempoWindowHopSeconds = 3.0` s hop, with `kMinimumTempoWindowSeconds = 8.0` s
   required to emit a window. Steps 3-7 below are shared with librosa.cpp.
2. Each window is passed to `sonare_analyze_bpm(..., bpm_min 40, bpm_max 240,
   start_bpm = fallback, n_fft 4096, hop 512, max_candidates 10, ...)`. The
   window's primary BPM and its candidate list (bpm + confidence) become one
   `TempoObservation` whose `center` is the window midpoint.
3. `selectTailHalfTempo()` handles one specific failure: on the final window
   (`window_end >= duration - 2.0`) with low confidence (`< 0.6`), if a candidate
   at roughly half the primary BPM scores within 85 % of the best, that candidate
   is selected instead. This is an anti-octave-error rule for fade-outs.
   A backend that already resolves octave errors per window must report a
   confidence that reflects that, or this rule will undo the decision — see the
   librosa.cpp notes below.
4. `resolveShortAmbiguousRuns()` looks at runs of ≤ 3 consecutive observations
   that disagree with their neighbours and re-picks, from the observation's own
   candidate list, whichever candidate (scoring ≥ 75 % of the best) is closest in
   log-BPM to the surrounding tempo. Runs at index 0 and the final run are not
   considered.
5. Consecutive observations are grouped into runs by `sameTempo()`, a **5 %
   relative tolerance**. Each run contributes one tempo point whose BPM is the
   *median of the selected BPMs in that run* (`medianSelectedBpm`).
6. The **time** of a tempo point is initially the midpoint between the last
   window centre of the previous run and the first window centre of this run — or
   `window_start` when the tail-half rule fired. The first point is forced to
   `t = 0`.
7. `refineTempoTransition()` then snaps that time, using the backend's onset
   envelope (libsonare: `sonare_onset_strength(..., n_fft 2048, hop 512,
   n_mels 128)`). For each
   frame in a ±6 s search band computes
   `localPeriodicity(onset, frame, ±2 s, lag_next) - localPeriodicity(..., lag_prev)`,
   smooths it over ±0.25 s, and takes the zero crossing nearest the coarse time.
   Points that came from the tail-half rule are skipped.

Output: `vector<pair<seconds, bpm>>`, first entry always at 0.

### Beat grid — `detectTempoAwareBeats()`

For each tempo segment `[tempoPoints[i].first, tempoPoints[i+1].first)` the
sub-span of audio is passed to `sonare_detect_beats()` and the returned
segment-local times are offset by the segment start. Beats closer than 50 ms to
the previous kept beat, or at/after the segment end, are dropped.

Because beat tracking is restarted per segment, **beat phase is re-derived
independently in each segment**; nothing forces continuity across a tempo change.

### Meter map — `uapmd_mir::estimateMeterMap()` (shared)

1. Candidate meters are enumerated by `meterCandidates()`: numerators
   `{3,4,5,6,7,8,9,11,12,13}`, each with a fixed set of accent groupings
   (e.g. 7 → `{3,2,2}`, `{2,3,2}`, `{2,2,3}`).
2. **The denominator is a hard-coded function of the numerator:**
   `numerator >= 5 && numerator != 12 ? 8 : 4`. There is no analysis behind it.
3. Per-beat "strength" (`uapmd_mir::beatStrengths()`) is the max of the onset
   envelope over ±2 frames around the beat, min-max normalized across the whole
   track.
4. A DP over beat indices segments the beat sequence. A segment may be at most
   `maxSegmentBeats = 96` beats, must be a whole number of bars
   (`length % numerator == 0`) and at least two bars long. Each candidate is
   scored by `scoreCandidate()`:
   - `mean(downbeat) - mean(weak)`
   - `+ 0.55 * (mean(secondary) - mean(weak))` where "secondary" are the
     group-start positions
   - `+ 0.35 * (1 - mean bar-pattern deviation)`
   - `+ 0.25 * (1 - beat-interval coefficient of variation)`
   times the segment length, minus a flat `transitionPenalty = 1.15` per
   non-initial segment.
5. Segments with identical numerator/denominator/grouping are merged; the
   reported time is `beats[start]`, i.e. the **first beat of the segment**, which
   is thereby *asserted* to be a downbeat. The first segment's time is forced
   to 0.

There is no downbeat detection anywhere in this. The DP chooses the bar phase
implicitly by choosing where segments begin.

### Chords

`sonare_detect_chords_ex()` with `min_duration 0.25`, `smoothing_window 0.5`,
`threshold 0.35`, `n_fft 4096`, `hop 512`, beat-sync on, HMM on (beam 8),
inversions on, `chroma_method 1`. Root/quality are mapped to a label by
`chordLabel()`; only `chord.start` is kept.

### Diagnostics

`MirDiagnosticLog` (in `MirDiagnostics.hpp`, used by both backends) buffers every
raw analyzer value in memory (1 MB of bytes,
16k records reserved) and flushes at the end of the run, grouped by kind
(`Calls`, `Tempo`, `Beats`, `Onsets`, `Meter`, `Chords`, `Other`) with a 25 ms
sleep every 32 lines so the logger is not swamped. Every libsonare call is logged
with its arguments and every returned value with full precision, including the
per-frame onset envelope. This is the intended way to compare a backend's raw
output against its post-processed output.

The librosa.cpp backend logs the same categories, with one deliberate omission:
it does not dump the onset envelope frame by frame. That dump is tens of
thousands of lines for a normal track, and `flush()` sleeps 25 ms every 32 lines,
so reproducing it would add roughly twenty seconds to every run.

## Backend B: librosa.cpp

`MirLibrosaAddin.cpp` orchestrates; `MirLibrosaAnalysis.cpp` supplies librosa's
primitives to the same shared code, in namespace `uapmd_mir::librosa_cpp`. It
exposes the same three entry points the libsonare backend does
(`detectTempoMap`, `detectRhythmMap`, `detectChords`), so the probe tool can run
either.

### Tempo map

There is no librosa call that returns tempo candidates: `librosa::beat::tempo()`
computes a scored lag axis and returns only its argmax. `analyzeTempoWindow()`
therefore reproduces that objective and keeps the whole peak list.

1. One onset envelope for the whole source
   (`onset_strength(mono, sr, n_fft 2048, hop 512)`).
2. Per window, one autocorrelation over exactly that window:
   `tempogram(window, sr, hop, win_length = window frames, center = false)`. The
   analysis window *is* the correlation window, so there is nothing to average
   over and no ramp padding inside it.
3. Score each lag the way `tempo()` does —
   `log1p(1e6 * correlation(lag))` plus a log-normal prior
   `-0.5 * ((log2(bpm) - log2(priorBpm)) / 1.0)^2` — restricted to
   40–240 BPM, the band the libsonare backend asks for.
4. Keep local maxima only (adjacent lags are the same tempo) and
   **parabolically interpolate** each peak across its neighbouring lags. Without
   this the BPM is quantized to the lag grid, which near 120 BPM is about 2.7 BPM
   wide — enough on its own to drift the beat grid off the audio within a minute.
5. Sort by score, keep the top 10, and convert scores to `exp(score - best)` so
   they read as likelihoods relative to the best peak, matching how the shared
   code's ratio tests treat libsonare's confidences.
6. `selectPrimaryCandidate()` picks the reported BPM: among candidates scoring
   within 10 % of the best, the one nearest the prior tempo. This is the step
   libsonare performs internally — its own candidate list shows the same
   inversion (for a 150 BPM window it ranks 74.5 above 150.0) while
   `SonareBpmAnalysisResult::bpm` still comes back as 150. The prior is already
   in the score, but log1p compression flattens the peaks too much for it to
   decide alone.
7. `windowConfidence()` reports doubt from the best candidate that is *not*
   octave-related to the primary, and only when it is a near-tie. Reporting
   octave rivals as doubt would let the shared `selectTailHalfTempo()` rule flip
   the octave decision step 6 just made.

The window scheduling, run grouping, short-run resolution and transition
refinement are then the shared `buildTempoMap()`.

### Beat grid

`detectTempoAwareBeats()` runs `beat_track` **per tempo-map segment**, over that
segment's slice of the onset envelope, with the segment's BPM as `start_bpm` and
`tightness = 100`, `trim = false`. Frame indices are offset by the segment's
first frame; the same 50 ms de-duplication and end-of-segment trim as the
libsonare backend apply.

`restoreLeadingBeats()` then back-extrapolates the grid at the opening tempo down
to t≈0. librosa's tracker can lock on well into the file, and the meter estimator
treats the first beat of its opening segment as a downbeat at t=0.

### Meter

Shared: `beatStrengths()` + `estimateMeterMap()`. The librosa backend previously
had a near-copy of the DP that omitted the beat-interval regularity term and the
±2-frame strength smoothing; that copy is gone.

### Chords

`chroma_stft(mono, sr, 4096, hop)`, then a 24-way major/minor template match with
weights `root/fifth 1.0, third 0.8, "wrong" third 0.2, everything else -0.2`.
Chroma is averaged over a `kChordSmoothingSeconds = 0.5` s window before the
argmax, and runs shorter than `kChordMinimumSeconds = 0.25` s are absorbed into
the chord they interrupted — the same two knobs the libsonare chord detector is
configured with. Previously the argmax ran per frame with no smoothing, emitting
a chord change as often as every ~12 ms.

There is still no HMM and no inversion detection; librosa.cpp has no equivalent
of `sonare_detect_chords_ex`.

### Side-by-side

| aspect | libsonare | librosa.cpp |
|---|---|---|
| tempo model | piecewise-constant map from 12 s / 3 s sliding windows | same (shared `buildTempoMap`) |
| per-window estimate | `sonare_analyze_bpm` | `tempogram` + `tempo()`'s objective, peaks parabolically interpolated |
| octave-error handling | inside libsonare, plus the shared tail-half rule | `selectPrimaryCandidate`, plus the shared tail-half rule |
| BPM precision | continuous | continuous (parabolic interpolation) |
| beat tracking | per tempo segment (`sonare_detect_beats`) | per tempo segment (`beat_track`) |
| beat grid before first detected beat | none | back-extrapolated |
| beat strength / meter DP | shared | shared |
| denominator | `n>=5 && n!=12 ? 8 : 4` (shared) | identical |
| chords | libsonare HMM detector, inversions | template match, 0.5 s smoothing, 0.25 s minimum |
| seconds→ticks | shared `tempoMapTimeToTicks` | same |
| diagnostics | full raw-value dump | same, minus the per-frame onset dump |

### Measured behaviour

Against synthetic click tracks through `uapmd-mir-tempo-probe`:

| case | truth | libsonare | librosa.cpp |
|---|---|---|---|
| constant tempo, 3/4, 60 s | 100.000 BPM | 99.806 | 99.974 |
| two-part track, first half | 120.000 BPM | 120.082 | 120.026 |
| two-part track, second half | 150.000 BPM | 150.218 | 150.001 |
| tempo-change time | 40.000 s | 40.147 s | 40.147 s |

Both now find the tempo change, and both place it at the same time — that error
is in the shared window scheduling, item 4 below. The BPM values are where
librosa.cpp is now the more accurate of the two, because of the parabolic peak
interpolation.

## What actually determines displayed timing

The analysis result is not what the ruler draws. Four more conversions happen:

1. `MidiClipSourceNode::computeSampleTimeline()` converts the tick positions back
   to samples using the clip's own `tempo_changes` — this is the exact inverse of
   `tempoMapTimeToTicks()`, so **the seconds → ticks → samples round trip is
   consistent** and is not itself a source of error.
2. `TimelineFacadeImpl::appendMidiNodeMetaToSnapshot()` converts those samples
   back to seconds and adds the clip's start sample, producing the
   timeline-absolute `MasterTrackSnapshot::tempoPoints` /
   `timeSignaturePoints`.
3. `uapmd::TempoMap::rebuild()` integrates the tempo points into quarter-note
   beats and stores each time signature's `startBeat = secondsToBeats(t)` — a
   **fractional** beat position.
4. `BeatsSequenceEditor::drawBarLines()` restarts the bar grid at each region's
   `startBeat`, stepping by `signatureBeatLength = 4.0 / denominator` quarter
   notes and drawing a bar line every `numerator` steps.

Two properties of that chain matter:

- **There is no downbeat anchor.** Beat 0 is *clip start*, full stop. A time
  signature's bar phase is whatever `startBeat` happens to be.
- **The bar length is `numerator * 4 / denominator` quarter notes.** A meter
  reported as 7/8 produces 3.5-quarter bars, whether or not the beats that were
  counted were quarters.

## Where the timing goes wrong

These are ordered by how much they can move an event, largest first.

### 1. The tempo map and the beat/meter map are never reconciled

This is the structural defect. The tempo map is estimated by autocorrelation over
12 s windows; the beat grid is estimated separately by a beat tracker; the meter
map is a segmentation *of the beat grid*. The final tick position of a
time-signature change is then computed by integrating the **tempo map** up to a
time that came from the **beat grid**.

Those two grids only agree if the tempo map's piecewise-constant BPM exactly
reproduces the tracked beat times, which it does not — `medianSelectedBpm()`
emits raw analyzer output such as 123.047 BPM with no snapping and no
phase. The beat implied by the tempo map drifts against the audible beat, and the
drift accumulates: **0.5 BPM of error at 120 BPM is 1.5 beats of drift after
three minutes.** Every time-signature change lands progressively further from the
downbeat it was measured at, and so does every bar line the region generates.

The parabolic peak interpolation in the librosa.cpp backend shrinks this by
roughly an order of magnitude (measured 0.026 BPM error against a true 100 BPM,
versus libsonare's 0.194) but does not remove it: 0.026 BPM is still a quarter of
a beat after ten minutes, and nothing ties the map's phase to a tracked beat.

A tempo map that is meant to drive a bar ruler has to be fitted *to* the beat
grid (each segment's BPM chosen so the segment spans a whole number of beats
between two tracked beats), not estimated independently of it.

### 2. Nothing establishes bar phase

Both backends force their first meter segment to `t = 0` and both let `TempoMap`
put beat 0 at clip start. Any pickup bar, count-in, or leading silence shifts
every bar line by that amount for the whole project. Tracking beats per tempo
segment — which both backends now do — compounds this: bar phase after a tempo
change is whatever the tracker happened to lock onto for that sub-span.

libsonare exposes `sonare_detect_downbeats()`. Neither backend calls it.

### 3. The denominator heuristic halves the bar

`numerator >= 5 && numerator != 12 ? 8 : 4`, in the shared `meterCandidates()`,
so it applies to both backends. The beats being counted are what the beat tracker
found — quarter-note level pulses under a quarter-note BPM. Labelling a 7-beat bar as 7/8 tells
`drawBarLines()` the bar is 3.5 quarter notes long, i.e. **half the length the
analysis actually measured**. Anything detected with numerator 5, 6, 7, 8, 9, 11
or 13 is wrong by a factor of two, and each such region's bar lines are wrong from
its very first bar rather than drifting into error.

libsonare's `sonare_analyze_rhythm()` returns a `SonareTimeSignature` with both
numerator and denominator plus a confidence, and `SonareAnalysisResult` carries
time-signature candidates. Neither is used; the meter estimator is hand-rolled
and shared.

### 4. Tempo-change times are quantized to the window grid

The coarse time of a tempo change is the midpoint between two window
centres, so it is quantized to `kTempoWindowHopSeconds / 2 = 1.5 s` and biased by up to
half the 12 s window. `refineTempoTransition()` searches ±6 s around it for a zero
crossing of a smoothed periodicity difference — that is the right idea, but the
smoothing radius (±0.25 s) and the ±2 s periodicity window mean it cannot resolve
better than roughly a second, and if the true transition is more than 6 s from the
coarse estimate it is not reachable at all. Additionally, the 5 %
`sameTempo` tolerance means a genuine 120 → 125 BPM change is not detected as a
change at all. Both backends measured the same 40.147 s for a change that
truly occurs at 40.000 s, which is this rule and the window grid, not the
backend.

### 5. Analysis coordinates are source-file coordinates, not clip coordinates

`readAudioSourceSamples(sourceId, 0, source->frameCount, ...)` reads the whole
file from frame 0, while the result is anchored at `sourceClip->position`. Any
clip that does not start at source frame 0, and any clip with
`audioWarps`, has its analysis times offset or stretched relative to what the
timeline plays. `AudioFileSourceNode` renders warped material and reports
`totalLength()` in the warped/resampled domain; the analysis sees none of that.

### 6. Sample-rate domain mix-up in the clip duration

```cpp
result.duration_samples = sourceClip->durationSamples;  // TIMELINE sample rate
result.sample_rate      = source->sampleRate;           // FILE sample rate
...
durationSeconds = duration_samples / sample_rate;
```

`ClipData::durationSamples` comes from `AudioFileSourceNode::totalLength()`, which
is explicitly documented as being in the *target* (engine) sample-rate domain,
while `source->sampleRate` is the file header's rate. A 44.1 kHz file in a 48 kHz
engine makes `durationSeconds` 8.8 % too long. This only affects the `endOfClip`
tick and hence the master clip's reported length — it does not move tempo or
time-signature events — but it is the same class of error and worth fixing
alongside them.

### Fixed since the first draft of this document

Two items no longer apply and are recorded here only so the history is legible:

- librosa.cpp used to hard-code `tempo_points = {{0.0, bpm}}` and convert
  seconds to ticks with that single BPM, so it could not represent a tempo
  change at all. It now builds a real tempo map through the shared code.
- `detectChords()` used to emit an event at every chroma frame whose argmax
  differed from the previous frame's — up to ~86 events per second at
  `hop = 512, sr = 44100`. It now smooths over 0.5 s and drops runs under
  0.25 s.

## Diagnostics and the probe tool

When `UAPMD_ENABLE_MIR`, `UAPMD_ENABLE_LIBSONARE` and `UAPMD_BUILD_TESTS` are on
(and not Android/iOS/Emscripten), CMake builds `uapmd-mir-tempo-probe` from
`tests/TempoMapProbe.cpp`. It links the analysis translation units plus
`sonare_core` and `librosa::librosa` — no engine, no app — and runs the tempo and
meter estimators of **either backend** against a WAV file:

```bash
cmake-build-debug/source/uapmd-mir/uapmd-mir-tempo-probe input.wav librosa
```

The second argument is `libsonare` (the default) or `librosa`. Running both over
the same file is the cheapest way to tell a backend-specific problem from one in
the shared code.

It accepts **little-endian 32-bit float WAV only** (`fmt` tag 3 or extensible
with subformat 3), downmixes to mono itself, and prints:

```
backend    <name>
tempo
<seconds>  <bpm>            <tick>
end        <duration>       <tick>
meter
<seconds>  <num>/<den>      <tick>
```

The tick column is `tempoMapTimeToTicks()` at TPQN 480, so the probe exercises
exactly the seconds → ticks step that `MirAddin.cpp` uses. This is the fastest
way to reproduce items 1–4 above without the GUI.

For the in-app path, `MirDiagnosticLog` output is the reference: it prints
`uapmd calling <backend> ...` before each call, `<backend> raw ...` for each
returned value, and `uapmd tempo-map output ...` / `uapmd meter-map output ...`
for the post-processed result, so raw and derived values can be diffed directly.
Both backends now emit this; only the per-frame onset dump is libsonare-only.

## Unused upstream capability

Worth recording, because a good deal of `uapmd-mir` reimplements things the
dependencies already provide:

- libsonare: `sonare_detect_downbeats()`, `sonare_analyze_rhythm()` (returns
  `SonareRhythmResult` with `time_signature`, `tempo_stability`,
  `pattern_regularity`, `beat_intervals`), `sonare_tempogram()`,
  `sonare_cyclic_tempogram()`, `sonare_plp()`, and — subject to which libsonare
  subsystems our CPM option set actually builds — `sonare_project_analyze_tempo()`
  and the project-level time-signature segment API.
- librosa.cpp: `librosa::beat::tempo(..., aggregate = false)` and
  `librosa::beat::tempo_frames()` for a per-frame tempo curve,
  `librosa::beat::plp()` for predominant local pulse — the natural source of a
  beat *phase*, which is what item 2 above is missing.

The shared meter estimator, and the sliding-window tempo map both backends now
use, predate any use of these.
