#include "AraFormatBinding.hpp"
#include "AraDocumentController.hpp"

#include <aap/core/aapxs/ara-aapxs.h>
#include <aap/core/host/plugin-instance.h>
#include <uapmd-data/uapmd-data.hpp>
#include <remidy/remidy.hpp>

#include <android/log.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace uapmd::ara {
    namespace {
        constexpr std::string_view kArchiveId = "uapmd-aap-ara-modifications-v1";

        // Independent file readers avoid accessing the mutable timeline on a Binder
        // thread, including when the control thread is waiting for that same request.
        class AapAudioSource final {
            ProjectAudioSourceSnapshot source;
            FileAudioSourceRepository repository;
        public:
            explicit AapAudioSource(ProjectAudioSourceSnapshot snapshot) : source(std::move(snapshot)) {}

            bool read(const aap_ara_audio_source_sample_range_t& range,
                      aap_ara_audio_source_samples_buffer_t& destination) const {
                const auto capacity = destination.data_size;
                destination.data_size = 0;
                destination.sample_count = 0;
                if (!destination.data || range.start_sample < 0 || range.sample_count < 0 ||
                    range.channel_count <= 0 || range.channel_count > source.channelCount ||
                    range.start_sample >= source.frameCount)
                    return false;
                const auto frames = std::min(range.sample_count, source.frameCount - range.start_sample);
                const auto width = range.sample_format == AAP_ARA_SAMPLE_FORMAT_FLOAT32 ? sizeof(float)
                    : range.sample_format == AAP_ARA_SAMPLE_FORMAT_FLOAT64 ? sizeof(double) : 0;
                if (!width || static_cast<uint64_t>(frames) > capacity / width / range.channel_count)
                    return false;
                std::vector<float> samples(static_cast<size_t>(frames) * range.channel_count);
                std::vector<float*> planes(range.channel_count);
                for (int32_t ch = 0; ch < range.channel_count; ++ch)
                    planes[ch] = samples.data() + static_cast<size_t>(ch) * frames;
                if (!source.filepath.empty() && !repository.readAudioSourceSamples(
                        source.audioSourceId, source.filepath, range.start_sample, frames,
                        planes.data(), range.channel_count))
                    return false;
                if (width == sizeof(float))
                    std::memcpy(destination.data, samples.data(), samples.size() * width);
                else
                    std::copy(samples.begin(), samples.end(), static_cast<double*>(destination.data));
                destination.data_size = samples.size() * width;
                destination.sample_count = frames;
                destination.channel_count = range.channel_count;
                destination.sample_format = range.sample_format;
                return true;
            }
        };

        class AapDocumentController final : public AraDocumentController {
            struct Source {
                int64_t id;
                int64_t modification_id;
                ProjectAudioSourceSnapshot snapshot;
                std::string name;
            };
            struct Sequence {
                int64_t id;
                int32_t order_index;
            };
            struct Region {
                int64_t id;
                ProjectObjectId source_id;
                ProjectClipSnapshot snapshot;
            };
            struct HostContext {
                aap_ara_host_extension_t extension{};
                std::mutex mutex;
                std::map<int64_t, std::shared_ptr<AapAudioSource>> sources;
                std::vector<aap_ara_content_update_t> updates;
                struct Target {
                    int64_t region_id;
                    int64_t source_id;
                    int64_t modification_id;
                    ProjectObjectId track_id;
                };
                std::vector<Target> targets;
                std::function<void(const ProjectObjectId&)> signal_changed;
                bool scheduled{};
                std::weak_ptr<HostContext> weak_self;
            };
            aap::RemotePluginInstance& instance;
            // An owned typed client serializes this document's calls independently.
            aap::xs::AraClientAAPXS client;
            aap_ara_extension_t* extension;
            AndroidAudioPlugin* plugin;
            aap_ara_factory_capability_t capability;
            std::shared_ptr<HostContext> host = std::make_shared<HostContext>();
            decltype(instance.getHostExtension) previous_host_extension;
            std::map<ProjectObjectId, Sequence> sequences;
            std::map<ProjectObjectId, Source> sources;
            std::map<ProjectObjectId, Region> regions;
            int64_t next_id{10};
            int edit_depth{};
            bool created{};
            AraRequestId next_request{1};

            bool supportsArchives() const {
                return capability.struct_size >= sizeof(capability) &&
                    (capability.supported_features & AAP_ARA_FEATURE_MODIFICATION_ARCHIVE);
            }

            static void getHostCapability(aap_ara_host_extension_t*, AndroidAudioPluginHost*,
                                          aap_ara_host_capability_t* destination) {
                if (!destination)
                    return;
                const auto capacity = destination->struct_size;
                const aap_ara_host_capability_t value{sizeof(value), AAP_ARA_API_GENERATION_2,
                    AAP_ARA_ROLE_PLAYBACK_RENDERER | AAP_ARA_ROLE_EDITOR_RENDERER | AAP_ARA_ROLE_EDITOR_VIEW,
                    AAP_ARA_SAMPLE_FORMAT_FLOAT32 | AAP_ARA_SAMPLE_FORMAT_FLOAT64,
                    AAP_ARA_HOST_SUPPORTS_CONTENT_UPDATES};
                std::memcpy(destination, &value, std::min<size_t>(capacity, sizeof(value)));
            }

            static void readSamples(aap_ara_host_extension_t* ext, AndroidAudioPluginHost*, int64_t id,
                                    const aap_ara_audio_source_sample_range_t* range,
                                    aap_ara_audio_source_samples_buffer_t* destination) {
                if (!range || !destination)
                    return;
                auto* context = static_cast<HostContext*>(ext->aapxs_context);
                std::shared_ptr<AapAudioSource> source;
                {
                    std::lock_guard lock(context->mutex);
                    if (auto it = context->sources.find(id); it != context->sources.end())
                        source = it->second;
                }
                if (source)
                    source->read(*range, *destination);
                else {
                    destination->data_size = 0;
                    destination->sample_count = 0;
                }
            }

            static void drainUpdates(HostContext& context) {
                std::vector<aap_ara_content_update_t> updates;
                std::vector<HostContext::Target> targets;
                std::function<void(const ProjectObjectId&)> signal_changed;
                {
                    std::lock_guard lock(context.mutex);
                    updates.swap(context.updates);
                    context.scheduled = false;
                    targets = context.targets;
                    signal_changed = context.signal_changed;
                }
                std::set<ProjectObjectId> dirty_tracks;
                for (const auto& update : updates) {
                    if (update.flags & AAP_ARA_CONTENT_SIGNAL_UNCHANGED)
                        continue;
                    for (const auto& target : targets)
                        if (update.kind == AAP_ARA_CONTENT_DOCUMENT ||
                            (update.kind == AAP_ARA_CONTENT_PLAYBACK_REGION && update.object_id == target.region_id) ||
                            (update.kind == AAP_ARA_CONTENT_AUDIO_SOURCE && update.object_id == target.source_id) ||
                            (update.kind == AAP_ARA_CONTENT_AUDIO_MODIFICATION && update.object_id == target.modification_id))
                            dirty_tracks.insert(target.track_id);
                }
                if (signal_changed)
                    for (const auto& track_id : dirty_tracks)
                        signal_changed(track_id);
            }

            bool storeModification(const Source& source, std::vector<uint8_t>& bytes) {
                bytes.resize(AAP_ARA_MAX_ARCHIVE_BYTES);
                aap_ara_archive_buffer_t buffer{sizeof(buffer), bytes.data(), bytes.size(), 0};
                if (!extension->store_audio_modification_state(extension, plugin, source.modification_id, &buffer) ||
                    buffer.data_size > bytes.size())
                    return false;
                bytes.resize(buffer.data_size);
                return true;
            }

            static void append(std::vector<uint8_t>& out, const void* data, size_t size) {
                const auto length = static_cast<uint32_t>(size);
                const auto* header = reinterpret_cast<const uint8_t*>(&length);
                out.insert(out.end(), header, header + sizeof(length));
                if (size) {
                    const auto* bytes = static_cast<const uint8_t*>(data);
                    out.insert(out.end(), bytes, bytes + size);
                }
            }

            static bool read(const std::vector<uint8_t>& in, size_t& offset, std::vector<uint8_t>& bytes) {
                uint32_t length;
                if (offset > in.size() || in.size() - offset < sizeof(length))
                    return false;
                std::memcpy(&length, in.data() + offset, sizeof(length));
                offset += sizeof(length);
                if (length > in.size() - offset)
                    return false;
                bytes.assign(in.begin() + offset, in.begin() + offset + length);
                offset += length;
                return true;
            }

            std::vector<ProjectObjectId> trackSources(const ProjectObjectId& track_id) const {
                std::vector<const Region*> clips;
                for (const auto& [_, region] : regions)
                    if (region.snapshot.trackId == track_id)
                        clips.push_back(&region);
                std::sort(clips.begin(), clips.end(), [](auto* a, auto* b) {
                    return a->snapshot.position.samples != b->snapshot.position.samples
                        ? a->snapshot.position.samples < b->snapshot.position.samples
                        : a->snapshot.clipNumericId < b->snapshot.clipNumericId;
                });
                std::vector<ProjectObjectId> result;
                for (auto* region : clips)
                    result.push_back(region->source_id);
                return result;
            }

        public:
            AapDocumentController(aap::RemotePluginInstance& instance,
                                  aap_ara_factory_capability_t capability, std::string name)
                : instance(instance)
                , client(instance.getAAPXSDispatcher().getPluginAAPXSByUri(AAP_ARA_EXTENSION_URI),
                         instance.getAAPXSDispatcher().getPluginAAPXSByUri(AAP_ARA_EXTENSION_URI)->serialization)
                , extension(client.asPluginExtension())
                , plugin(instance.getPlugin())
                , capability(capability)
                , previous_host_extension(instance.getHostExtension) {
                host->extension = {host.get(), getHostCapability, readSamples, nullptr};
                host->weak_self = host;
                host->extension.notify_content_changed = [](aap_ara_host_extension_t* ext, AndroidAudioPluginHost*,
                                                            const aap_ara_content_update_t* update) {
                    if (!update || update->struct_size < sizeof(*update))
                        return;
                    auto* context = static_cast<HostContext*>(ext->aapxs_context);
                    bool schedule;
                    {
                        std::lock_guard lock(context->mutex);
                        context->updates.push_back(*update);
                        schedule = !context->scheduled;
                        context->scheduled = true;
                    }
                    // No plugin call or timeline mutation is made in this callback.
                    if (schedule) {
                        remidy::EventLoop::enqueueTaskOnMainThread([weak = context->weak_self] {
                            if (auto context = weak.lock())
                                drainUpdates(*context);
                        });
                    }
                };
                instance.getHostExtension = [context = host, previous = previous_host_extension]
                    (aap::RemotePluginInstance* owner, uint8_t urid, const char* uri) -> void* {
                    if (uri && std::strcmp(uri, AAP_ARA_EXTENSION_URI) == 0)
                        return &context->extension;
                    return previous ? previous(owner, urid, uri) : nullptr;
                };
                beginProjectDocumentTransaction();
                aap_ara_document_properties_t doc{sizeof(doc), name.c_str(), "uapmd.document"};
                aap_ara_musical_context_properties_t context{sizeof(context), "Master", nullptr, "uapmd.master"};
                extension->create_document(extension, plugin, 1, &doc);
                extension->create_musical_context(extension, plugin, 1, 2, &context);
                created = true;
                endProjectDocumentTransaction();
            }

            ~AapDocumentController() override {
                {
                    std::lock_guard lock(host->mutex);
                    host->signal_changed = nullptr;
                    host->targets.clear();
                }
                if (created) {
                    if (!edit_depth)
                        beginProjectDocumentTransaction();
                    for (const auto& [_, region] : regions)
                        extension->destroy_playback_region(extension, plugin, region.id);
                    for (const auto& [_, source] : sources) {
                        extension->destroy_audio_modification(extension, plugin, source.modification_id);
                        extension->enable_audio_source_samples_access(extension, plugin, source.id, false);
                        extension->destroy_audio_source(extension, plugin, source.id);
                    }
                    for (const auto& [_, sequence] : sequences)
                        extension->destroy_region_sequence(extension, plugin, sequence.id);
                    extension->destroy_musical_context(extension, plugin, 2);
                    extension->destroy_document(extension, plugin, 1);
                    edit_depth = 1;
                    endProjectDocumentTransaction();
                }
                instance.getHostExtension = std::move(previous_host_extension);
            }

            bool valid() const override { return created; }
            ARA::ARADocumentControllerRef documentControllerRef() const override { return nullptr; }
            const ARA::ARAFactory* factory() const override { return nullptr; }
            void bindPluginExtension(const ARA::ARAPlugInExtensionInstance*) override {}
            void setRenderedSignalChangedCallback(std::function<void(const ProjectObjectId&)> callback) override {
                std::lock_guard lock(host->mutex);
                host->signal_changed = std::move(callback);
            }
            void beginProjectDocumentTransaction() override {
                if (edit_depth++ == 0)
                    extension->begin_model_update(extension, plugin, AAP_ARA_MODEL_UPDATE_FLAG_NONE);
            }
            void endProjectDocumentTransaction() override {
                if (edit_depth > 0 && --edit_depth == 0) {
                    extension->end_model_update(extension, plugin);
                    drainUpdates(*host);
                }
            }

            bool resyncFromProjectDocument(ProjectDocumentView& view, const TimelineFacade::MasterTrackSnapshot&) override {
                // Keep existing object IDs and modifications across edits and full resyncs.
                std::map<ProjectObjectId, ProjectTrackSnapshot> wanted_tracks;
                std::map<ProjectObjectId, ProjectAudioSourceSnapshot> wanted_sources;
                std::map<ProjectObjectId, ProjectClipSnapshot> wanted_clips;
                for (const auto& id : view.trackIds())
                    if (auto track = view.getTrack(id); track && !track->masterTrack) {
                        wanted_tracks.emplace(id, *track);
                        for (const auto& clip_id : view.clipIds(id))
                            if (auto clip = view.getClip(clip_id); clip && clip->clipType == ClipType::Audio)
                                wanted_clips.emplace(clip_id, *clip);
                    }
                for (const auto& id : view.audioSourceIds())
                    if (auto source = view.getAudioSource(id); source &&
                        source->channelCount > 0 && source->sampleRate > 0 && source->frameCount >= 0)
                        wanted_sources.emplace(id, *source);
                std::map<ProjectObjectId, ProjectObjectId> clip_sources;
                for (const auto& [id, source] : wanted_sources)
                    for (const auto& [clip_id, clip] : wanted_clips)
                        if ((!source.filepath.empty() && source.filepath == clip.filepath) || source.clipId == clip_id)
                            clip_sources[clip_id] = id;
                for (auto it = wanted_sources.begin(); it != wanted_sources.end();) {
                    const auto used = std::any_of(clip_sources.begin(), clip_sources.end(),
                        [&](const auto& entry) { return entry.second == it->first; });
                    if (!used)
                        it = wanted_sources.erase(it);
                    else
                        ++it;
                }
                beginProjectDocumentTransaction();
                for (auto it = regions.begin(); it != regions.end();) {
                    if (!wanted_clips.contains(it->first) || !clip_sources.contains(it->first) ||
                        it->second.source_id != clip_sources.at(it->first)) {
                        extension->destroy_playback_region(extension, plugin, it->second.id);
                        it = regions.erase(it);
                    } else
                        ++it;
                }
                for (auto it = sources.begin(); it != sources.end();) {
                    if (!wanted_sources.contains(it->first)) {
                        auto& source = it->second;
                        extension->destroy_audio_modification(extension, plugin, source.modification_id);
                        extension->enable_audio_source_samples_access(extension, plugin, source.id, false);
                        extension->destroy_audio_source(extension, plugin, source.id);
                        {
                            std::lock_guard lock(host->mutex);
                            host->sources.erase(source.id);
                        }
                        it = sources.erase(it);
                    } else
                        ++it;
                }
                // A sequence can only be destroyed after surviving regions have moved.
                for (const auto& [id, track] : wanted_tracks) {
                    const auto name = "Track " + std::to_string(track.trackIndex);
                    aap_ara_region_sequence_properties_t props{sizeof(props), name.c_str(), track.trackIndex,
                        2, nullptr, id.c_str()};
                    if (auto it = sequences.find(id); it != sequences.end()) {
                        if (it->second.order_index != track.trackIndex) {
                            extension->update_region_sequence_properties(extension, plugin, it->second.id, &props);
                            it->second.order_index = track.trackIndex;
                        }
                    } else {
                        const auto sequence_id = next_id++;
                        extension->create_region_sequence(extension, plugin, 1, sequence_id, &props);
                        sequences.emplace(id, Sequence{sequence_id, track.trackIndex});
                    }
                }
                std::set<ProjectObjectId> changed_sources;
                for (const auto& [id, snapshot] : wanted_sources) {
                    auto it = sources.find(id);
                    const bool added = it == sources.end();
                    if (added)
                        it = sources.emplace(id, Source{next_id++, next_id++, snapshot}).first;
                    auto& source = it->second;
                    const bool changed = added || source.snapshot.filepath != snapshot.filepath ||
                        source.snapshot.sampleRate != snapshot.sampleRate || source.snapshot.frameCount != snapshot.frameCount ||
                        source.snapshot.channelCount != snapshot.channelCount;
                    source.snapshot = snapshot;
                    auto clip_it = std::find_if(clip_sources.begin(), clip_sources.end(),
                        [&](const auto& entry) { return entry.second == id; });
                    const auto& name = wanted_clips.at(clip_it->first).name;
                    const bool renamed = source.name != name;
                    source.name = name;
                    aap_ara_audio_source_properties_t props{sizeof(props), name.c_str(), id.c_str(), snapshot.frameCount,
                        snapshot.sampleRate, static_cast<int32_t>(snapshot.channelCount), false, nullptr};
                    if (changed) {
                        changed_sources.insert(id);
                        std::lock_guard lock(host->mutex);
                        host->sources[source.id] = std::make_shared<AapAudioSource>(snapshot);
                    }
                    if (added) {
                        extension->create_audio_source(extension, plugin, 1, source.id, &props);
                        aap_ara_audio_modification_properties_t mod{sizeof(mod), name.c_str(), id.c_str()};
                        extension->create_audio_modification(extension, plugin, source.id, source.modification_id, &mod);
                        extension->enable_audio_source_samples_access(extension, plugin, source.id, true);
                    } else if (changed || renamed) {
                        extension->update_audio_source_properties(extension, plugin, source.id, &props);
                        if (renamed) {
                            aap_ara_audio_modification_properties_t mod{sizeof(mod), name.c_str(), id.c_str()};
                            extension->update_audio_modification_properties(extension, plugin, source.modification_id, &mod);
                        }
                        if (changed)
                            extension->notify_audio_source_content_changed(extension, plugin, source.id, 0, snapshot.frameCount);
                    }
                }
                for (const auto& [id, snapshot] : wanted_clips) {
                    if (!clip_sources.contains(id))
                        continue;
                    const auto& source_id = clip_sources.at(id);
                    const auto& source = sources.at(source_id);
                    const auto rate = snapshot.sampleRate > 0 ? snapshot.sampleRate : source.snapshot.sampleRate;
                    const auto start = static_cast<double>(snapshot.position.samples) / rate;
                    const auto duration = std::max(0.0, static_cast<double>(snapshot.durationSamples) / rate);
                    const auto source_duration = static_cast<double>(source.snapshot.frameCount) / source.snapshot.sampleRate;
                    const auto head_trim = -std::min(0.0, start);
                    const auto offset = std::clamp(static_cast<double>(snapshot.sourceOffsetSamples) / rate + head_trim,
                        0.0, source_duration);
                    aap_ara_playback_region_properties_t props{sizeof(props), 0, offset,
                        std::max(0.0, std::min(duration - head_trim, source_duration - offset)), std::max(0.0, start),
                        std::max(0.0, duration - head_trim), sequences.at(snapshot.trackId).id, snapshot.name.c_str(), nullptr};
                    if (auto it = regions.find(id); it != regions.end()) {
                        const auto& previous = it->second.snapshot;
                        if (previous.position.samples != snapshot.position.samples ||
                            previous.durationSamples != snapshot.durationSamples ||
                            previous.sourceOffsetSamples != snapshot.sourceOffsetSamples || previous.sampleRate != snapshot.sampleRate ||
                            previous.name != snapshot.name || previous.trackId != snapshot.trackId ||
                            changed_sources.contains(source_id))
                            extension->update_playback_region_properties(extension, plugin, it->second.id, &props);
                        it->second.snapshot = snapshot;
                    } else {
                        const auto region_id = next_id++;
                        extension->create_playback_region(extension, plugin, source.modification_id, region_id, &props);
                        regions.emplace(id, Region{region_id, source_id, snapshot});
                    }
                }
                for (auto it = sequences.begin(); it != sequences.end();) {
                    if (!wanted_tracks.contains(it->first)) {
                        extension->destroy_region_sequence(extension, plugin, it->second.id);
                        it = sequences.erase(it);
                    } else
                        ++it;
                }
                {
                    std::lock_guard lock(host->mutex);
                    host->targets.clear();
                    for (const auto& [_, region] : regions) {
                        const auto& source = sources.at(region.source_id);
                        host->targets.push_back({region.id, source.id, source.modification_id, region.snapshot.trackId});
                    }
                }
                endProjectDocumentTransaction();
                return true;
            }

            bool applyProjectDocumentEvent(ProjectDocumentView& view, const TimelineFacade::MasterTrackSnapshot& master,
                                           const ProjectDocumentEvent& event) override {
                const auto result = resyncFromProjectDocument(view, master);
                if (event.kind() == ProjectDocumentEventKind::AudioSourceChanged && event.audioSourceId())
                    if (auto it = sources.find(*event.audioSourceId()); it != sources.end()) {
                        beginProjectDocumentTransaction();
                        extension->notify_audio_source_content_changed(extension, plugin, it->second.id, 0,
                            it->second.snapshot.frameCount);
                        endProjectDocumentTransaction();
                    }
                return result;
            }

            // The current AAP binding exposes sample access, not analysis/content readers.
            AraRequestId requestAnalysis(AraAnalysisRequest request, AraAnalysisCallback callback) override {
                const auto id = next_request++;
                if (callback)
                    callback({id, request.objectId, {}, AraStatus::UnsupportedPlugin,
                        "AAP ARA does not expose analysis requests or content readers yet."});
                return id;
            }
            void cancelAnalysis(AraRequestId) override {}
            std::optional<AraContentEvents> readContent(AraContentScope, const ProjectObjectId&, AraContentKind) override {
                return std::nullopt;
            }
            void notifyModelUpdates() override { drainUpdates(*host); }

            bool saveArchiveState(std::vector<uint8_t>& archive, std::string& archive_id) override {
                archive.clear();
                archive_id = kArchiveId;
                if (!supportsArchives())
                    return true;
                if (edit_depth)
                    return false;
                for (const auto& [id, source] : sources) {
                    std::vector<uint8_t> bytes;
                    if (!storeModification(source, bytes))
                        return false;
                    // Clip identities survive project extraction to a new filesystem path.
                    auto region = std::find_if(regions.begin(), regions.end(),
                        [&](const auto& entry) { return entry.second.source_id == id; });
                    if (region == regions.end())
                        continue;
                    append(archive, region->first.data(), region->first.size());
                    append(archive, bytes.data(), bytes.size());
                }
                return true;
            }

            bool loadArchiveState(const std::vector<uint8_t>& archive, const std::string& archive_id) override {
                if (archive.empty())
                    return true;
                if (!supportsArchives() || archive_id != kArchiveId)
                    return false;
                std::map<ProjectObjectId, std::vector<uint8_t>> entries;
                size_t offset = 0;
                while (offset < archive.size()) {
                    std::vector<uint8_t> key, bytes;
                    if (!read(archive, offset, key) || !read(archive, offset, bytes) ||
                        key.empty() || key.size() > 65536 || bytes.size() > AAP_ARA_MAX_ARCHIVE_BYTES)
                        return false;
                    entries.emplace(std::string(key.begin(), key.end()), std::move(bytes));
                }
                beginProjectDocumentTransaction();
                bool result = true;
                for (const auto& [id, bytes] : entries)
                    if (auto it = regions.find(id); it != regions.end())
                        result &= extension->restore_audio_modification_state(extension, plugin,
                            sources.at(it->second.source_id).modification_id, bytes.data(), bytes.size());
                endProjectDocumentTransaction();
                return result;
            }

            bool storeArchiveStateForClip(const ProjectObjectId& clip_id, std::string& archive_id,
                                          std::string& source_id, std::string& modification_id,
                                          std::vector<uint8_t>& archive) override {
                archive.clear();
                archive_id = kArchiveId;
                if (!supportsArchives() || !regions.contains(clip_id))
                    return true;
                if (edit_depth)
                    return false;
                source_id = regions.at(clip_id).source_id;
                modification_id = source_id;
                return storeModification(sources.at(source_id), archive);
            }

            bool restoreArchiveStateForClip(const ProjectObjectId& clip_id, const std::string& archive_id,
                                            const std::string&, const std::string&, const std::vector<uint8_t>& archive) override {
                if (archive_id != kArchiveId || !supportsArchives() ||
                    archive.size() > AAP_ARA_MAX_ARCHIVE_BYTES || !regions.contains(clip_id))
                    return false;
                beginProjectDocumentTransaction();
                const auto result = extension->restore_audio_modification_state(extension, plugin,
                    sources.at(regions.at(clip_id).source_id).modification_id, archive.data(), archive.size());
                endProjectDocumentTransaction();
                return result;
            }

            bool storeArchiveStateForTrack(const ProjectObjectId& track_id, std::string& archive_id,
                                           std::string& sequence_id, std::vector<uint8_t>& archive) override {
                archive.clear();
                archive_id = kArchiveId;
                sequence_id = track_id;
                if (!supportsArchives())
                    return true;
                if (edit_depth)
                    return false;
                for (const auto& id : trackSources(track_id)) {
                    std::vector<uint8_t> bytes;
                    if (!storeModification(sources.at(id), bytes))
                        return false;
                    append(archive, bytes.data(), bytes.size());
                }
                return true;
            }

            bool restoreArchiveStateForTrack(const ProjectObjectId& track_id, const std::string& archive_id,
                                             const std::string&, const std::vector<uint8_t>& archive) override {
                if (!supportsArchives() || archive_id != kArchiveId)
                    return false;
                const auto ids = trackSources(track_id);
                std::vector<std::vector<uint8_t>> entries;
                size_t offset = 0;
                while (offset < archive.size()) {
                    std::vector<uint8_t> bytes;
                    if (!read(archive, offset, bytes) || bytes.size() > AAP_ARA_MAX_ARCHIVE_BYTES)
                        return false;
                    entries.push_back(std::move(bytes));
                }
                if (entries.size() != ids.size())
                    return false;
                beginProjectDocumentTransaction();
                bool result = true;
                for (size_t i = 0; i < ids.size(); ++i)
                    result &= extension->restore_audio_modification_state(extension, plugin,
                        sources.at(ids[i]).modification_id, entries[i].data(), entries[i].size());
                endProjectDocumentTransaction();
                return result;
            }
        };

        class AapFormatBinding final : public AraFormatBinding {
            aap::RemotePluginInstance& instance;
            aap_ara_factory_capability_t capability;
        public:
            AapFormatBinding(aap::RemotePluginInstance& instance, aap_ara_factory_capability_t capability)
                : instance(instance), capability(capability) {}
            std::string_view formatName() const override { return "AAP"; }
            const ARA::ARAFactory* factory() const override { return nullptr; }
            const ARA::ARAPlugInExtensionInstance* bindToDocumentController(
                ARA::ARADocumentControllerRef, ARA::ARAPlugInInstanceRoleFlags, ARA::ARAPlugInInstanceRoleFlags) override {
                return nullptr;
            }
            std::unique_ptr<AraDocumentController> createDocumentController(std::string name) override {
                return std::make_unique<AapDocumentController>(instance, capability, std::move(name));
            }
        };
    }

    std::unique_ptr<AraFormatBinding> createAapAraBinding(AraPluginInstanceHandleExtension& handles) {
        auto* instance = static_cast<aap::RemotePluginInstance*>(
            handles.nativeHandle(AraPluginInstanceHandleKind::AAPRemotePluginInstance));
        if (!instance) {
            __android_log_print(ANDROID_LOG_DEBUG, "uapmd-ara", "No AAP remote instance handle");
            return nullptr;
        }
        // The dispatcher also contains host-registered definitions. Require the
        // plugin's advertised support before creating an ARA client.
        auto* information = instance->getPluginInformation();
        if (!information || !information->hasExtension(AAP_ARA_EXTENSION_URI))
            return nullptr;
        auto* plugin = instance->getPlugin();
        auto* initiator = instance->getAAPXSDispatcher().getPluginAAPXSByUri(AAP_ARA_EXTENSION_URI);
        // AAP's proxy resolver requires a configured dispatcher entry.
        if (!plugin || !initiator || !initiator->serialization)
            return nullptr;
        auto* extension = static_cast<aap_ara_extension_t*>(plugin->get_extension(plugin, AAP_ARA_EXTENSION_URI));
        if (!extension) {
            __android_log_print(ANDROID_LOG_WARN, "uapmd-ara", "AAP ARA extension unavailable for instance %d", instance->getInstanceId());
            return nullptr;
        }
        aap::xs::AraClientAAPXS client(initiator, initiator->serialization);
        aap_ara_factory_capability_t capability{sizeof(capability)};
        client.getFactoryCapability(capability);
        __android_log_print(ANDROID_LOG_DEBUG, "uapmd-ara", "AAP ARA instance=%d capability size=%u generations=%u features=%u",
            instance->getInstanceId(), capability.struct_size, capability.api_generations, capability.supported_features);
        if (!(capability.api_generations & AAP_ARA_API_GENERATION_2))
            return nullptr;
        return std::make_unique<AapFormatBinding>(*instance, capability);
    }
}
