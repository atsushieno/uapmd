#include <algorithm>
#include <cmath>

#include <uapmd-data/uapmd-data.hpp>

namespace uapmd {

    namespace {

        void appendClipMeta(MasterTimelineMeta& meta,
                            const ClipData& clip,
                            MidiClipSourceNode& midiNode,
                            double sampleRate) {
            const double clipStartSamples = static_cast<double>(clip.position.samples);

            const auto& tempoSamples = midiNode.tempoChangeSamples();
            const auto& tempoEvents = midiNode.tempoChanges();
            const size_t tempoCount = std::min(tempoSamples.size(), tempoEvents.size());
            for (size_t i = 0; i < tempoCount; ++i) {
                MasterTimelineMeta::TempoPoint point;
                point.timeSeconds = (clipStartSamples + static_cast<double>(tempoSamples[i])) / sampleRate;
                point.tickPosition = tempoEvents[i].tickPosition;
                point.bpm = tempoEvents[i].bpm;
                meta.maxTimeSeconds = std::max(meta.maxTimeSeconds, point.timeSeconds);
                meta.tempoPoints.push_back(point);
            }

            const auto& sigSamples = midiNode.timeSignatureChangeSamples();
            const auto& sigEvents = midiNode.timeSignatureChanges();
            const size_t sigCount = std::min(sigSamples.size(), sigEvents.size());
            for (size_t i = 0; i < sigCount; ++i) {
                MasterTimelineMeta::TimeSignaturePoint point;
                point.timeSeconds = (clipStartSamples + static_cast<double>(sigSamples[i])) / sampleRate;
                point.tickPosition = sigEvents[i].tickPosition;
                point.signature = sigEvents[i];
                meta.maxTimeSeconds = std::max(meta.maxTimeSeconds, point.timeSeconds);
                meta.timeSignaturePoints.push_back(point);
            }
        }

    } // namespace

    MasterTimelineMeta buildMasterTimelineMeta(
        const std::shared_ptr<TimelineTrack>& masterTrack,
        double sampleRate,
        double defaultBpm
    ) {
        MasterTimelineMeta meta;
        const double sr = std::max(1.0, sampleRate);

        if (masterTrack) {
            auto clips = masterTrack->clipManager().getAllClips();
            std::sort(clips.begin(), clips.end(), [](const ClipData& a, const ClipData& b) {
                return a.clipId < b.clipId;
            });

            for (const auto& clip : clips) {
                if (clip.clipType != ClipType::Midi)
                    continue;
                auto sourceNode = masterTrack->getSourceNode(clip.sourceNodeInstanceId);
                auto* midiNode = dynamic_cast<MidiClipSourceNode*>(sourceNode.get());
                if (!midiNode)
                    continue;
                appendClipMeta(meta, clip, *midiNode, sr);
            }
        }

        std::stable_sort(meta.tempoPoints.begin(), meta.tempoPoints.end(),
            [](const MasterTimelineMeta::TempoPoint& a, const MasterTimelineMeta::TempoPoint& b) {
                return a.timeSeconds < b.timeSeconds;
            });
        std::stable_sort(meta.timeSignaturePoints.begin(), meta.timeSignaturePoints.end(),
            [](const MasterTimelineMeta::TimeSignaturePoint& a, const MasterTimelineMeta::TimeSignaturePoint& b) {
                return a.timeSeconds < b.timeSeconds;
            });

        std::vector<TempoMap::TempoPoint> mapTempo;
        mapTempo.reserve(meta.tempoPoints.size());
        for (const auto& point : meta.tempoPoints)
            mapTempo.push_back({point.timeSeconds, point.bpm});
        std::vector<TempoMap::TimeSignaturePoint> mapSignatures;
        mapSignatures.reserve(meta.timeSignaturePoints.size());
        for (const auto& point : meta.timeSignaturePoints)
            mapSignatures.push_back({point.timeSeconds, point.signature});
        meta.tempoMap.rebuild(mapTempo, mapSignatures, defaultBpm);

        return meta;
    }

}
