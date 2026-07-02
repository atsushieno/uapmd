#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#include "../midi/MidiTimelineEvents.hpp"

namespace uapmd {

    // Converts between real-world seconds and quarter-note beats using a piecewise-constant
    // tempo map, and exposes the time-signature-in-effect over each beat range.
    //
    // "Beat" here always means a quarter note, matching BPM (quarter notes per minute) and
    // MIDI tick-resolution (ticks per quarter note) conventions used elsewhere in the codebase.
    class TempoMap {
    public:
        struct TempoPoint {
            double timeSeconds{0.0};
            double bpm{120.0};
        };
        struct TimeSignaturePoint {
            double timeSeconds{0.0};
            MidiTimeSignatureChange signature{};
        };
        struct EffectiveSignature {
            double startBeat{0.0};
            double endBeat{std::numeric_limits<double>::infinity()};
            uint8_t numerator{4};
            uint8_t denominator{4};
        };

        void rebuild(const std::vector<TempoPoint>& tempoPoints,
                     const std::vector<TimeSignaturePoint>& timeSignaturePoints,
                     double defaultBpm = 120.0) {
            tempoSegments_.clear();
            effectiveSignatures_.clear();

            if (tempoPoints.empty()) {
                hasTempoData_ = false;
            } else {
                hasTempoData_ = true;

                double currentBpm = tempoPoints.front().bpm > 0.0 ? tempoPoints.front().bpm : defaultBpm;
                double lastTime = 0.0;
                double accumulatedBeats = 0.0;

                for (const auto& point : tempoPoints) {
                    double eventTime = std::max(0.0, point.timeSeconds);
                    if (eventTime > lastTime) {
                        const double bpmToUse = currentBpm > 0.0 ? currentBpm : defaultBpm;
                        tempoSegments_.push_back(TempoSegment{lastTime, eventTime, bpmToUse, accumulatedBeats});
                        accumulatedBeats += (eventTime - lastTime) * (bpmToUse / 60.0);
                        lastTime = eventTime;
                    }
                    if (point.bpm > 0.0)
                        currentBpm = point.bpm;
                }

                const double bpmToUse = currentBpm > 0.0 ? currentBpm : defaultBpm;
                tempoSegments_.push_back(TempoSegment{
                    lastTime,
                    std::numeric_limits<double>::infinity(),
                    bpmToUse,
                    accumulatedBeats
                });
            }

            for (const auto& point : timeSignaturePoints) {
                EffectiveSignature sig;
                sig.startBeat = secondsToBeats(std::max(0.0, point.timeSeconds));
                sig.numerator = point.signature.numerator;
                sig.denominator = point.signature.denominator;
                effectiveSignatures_.push_back(sig);
            }
            std::sort(effectiveSignatures_.begin(), effectiveSignatures_.end(),
                [](const EffectiveSignature& a, const EffectiveSignature& b) { return a.startBeat < b.startBeat; });
            for (size_t i = 0; i < effectiveSignatures_.size(); ++i) {
                effectiveSignatures_[i].endBeat = (i + 1 < effectiveSignatures_.size())
                    ? effectiveSignatures_[i + 1].startBeat
                    : std::numeric_limits<double>::infinity();
            }
        }

        void clear() {
            tempoSegments_.clear();
            effectiveSignatures_.clear();
            hasTempoData_ = false;
        }

        bool empty() const { return tempoSegments_.empty(); }
        bool hasTempoData() const { return hasTempoData_; }

        double secondsToBeats(double seconds) const {
            if (tempoSegments_.empty())
                return std::max(0.0, seconds);

            const double clampedSeconds = std::max(0.0, seconds);
            for (const auto& segment : tempoSegments_) {
                if (clampedSeconds < segment.endTime) {
                    const double bpm = segment.bpm > 0.0 ? segment.bpm : kDefaultBpm;
                    return segment.accumulatedBeats + (clampedSeconds - segment.startTime) * (bpm / 60.0);
                }
            }

            const auto& last = tempoSegments_.back();
            const double bpm = last.bpm > 0.0 ? last.bpm : kDefaultBpm;
            return last.accumulatedBeats + (clampedSeconds - last.startTime) * (bpm / 60.0);
        }

        double beatsToSeconds(double beats) const {
            if (tempoSegments_.empty())
                return std::max(0.0, beats);

            const double clampedBeats = std::max(0.0, beats);
            for (const auto& segment : tempoSegments_) {
                const double bpm = segment.bpm > 0.0 ? segment.bpm : kDefaultBpm;
                double segmentEndBeats = std::numeric_limits<double>::infinity();
                if (std::isfinite(segment.endTime))
                    segmentEndBeats = segment.accumulatedBeats + (segment.endTime - segment.startTime) * (bpm / 60.0);

                if (clampedBeats < segmentEndBeats)
                    return segment.startTime + (clampedBeats - segment.accumulatedBeats) * (60.0 / bpm);
            }

            const auto& last = tempoSegments_.back();
            const double bpm = last.bpm > 0.0 ? last.bpm : kDefaultBpm;
            return last.startTime + (clampedBeats - last.accumulatedBeats) * (60.0 / bpm);
        }

        const std::vector<EffectiveSignature>& effectiveSignatures() const { return effectiveSignatures_; }

    private:
        static constexpr double kDefaultBpm = 120.0;

        struct TempoSegment {
            double startTime{0.0};
            double endTime{0.0};
            double bpm{120.0};
            double accumulatedBeats{0.0};
        };

        std::vector<TempoSegment> tempoSegments_;
        std::vector<EffectiveSignature> effectiveSignatures_;
        bool hasTempoData_{false};
    };

} // namespace uapmd
