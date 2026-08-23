#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <uapmd-engine/uapmd-engine.hpp>

#include "NoteClipWriter.hpp"

// The parts of "transcribe an audio clip" that have nothing to do with which
// estimator does the transcribing: finding the clips, reading their samples,
// and writing the result back beside them.
//
// Compiled into each backend rather than into a library of its own, for the
// same reason as NoteClipWriter: the backends end up in separate binaries.
namespace uapmd_pitch {

// An audio clip paired with the audio source behind it. The document view
// indexes sources by their own id, so finding the source for a clip means
// walking them.
struct AudioClipSource {
    uapmd::ProjectClipSnapshot clip;
    uapmd::ProjectAudioSourceSnapshot source;
};

// Every audio clip in the project, or just the one named. Ordered by track and
// then position so a whole-project run progresses predictably.
std::vector<AudioClipSource> collectAudioClipSources(
    const uapmd::ProjectDocumentView& view,
    std::optional<int32_t> only_track_index = std::nullopt,
    std::optional<int32_t> only_clip_id = std::nullopt);

// Reads a source and downmixes it to mono at its own sample rate.
bool readMono(const uapmd::ProjectDocumentView& view,
              const uapmd::ProjectAudioSourceSnapshot& source,
              std::vector<float>& mono);

// Band-limited resampling. Downsampling with plain interpolation folds
// everything above the new Nyquist back into the audible band, which a pitch
// estimator then reads as real partials, so the sinc kernel is cut off at
// whichever Nyquist is lower.
std::vector<float> resample(const std::vector<float>& input,
                            double source_rate,
                            double target_rate);

// Adds `notes` as a MIDI 2.0 clip at the audio clip's position on its track,
// resized to the audio clip's length so there is room to draw automation over
// the whole of it. Returns false when the clip could not be added.
bool writeNoteClip(uapmd::SequencerEngine& engine,
                   const AudioClipSource& audio,
                   const std::vector<TranscribedNote>& notes,
                   std::string_view name_prefix);

} // namespace uapmd_pitch
