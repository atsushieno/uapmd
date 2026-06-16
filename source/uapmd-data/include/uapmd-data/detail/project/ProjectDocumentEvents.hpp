#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace uapmd {

    using ProjectRevision = uint64_t;
    using ProjectObjectId = std::string;
    using ProjectDocumentEventListenerToken = uint64_t;

    enum class ProjectDocumentEventKind {
        ProjectLoaded,
        ProjectClosing,
        ProjectSaved,

        MasterTrackChanged,

        TrackAdded,
        TrackRemoved,
        TrackChanged,

        ClipAdded,
        ClipRemoved,
        ClipChanged,

        AudioSourceAdded,
        AudioSourceRemoved,
        AudioSourceChanged,

        PluginGraphChanged
    };

    using ProjectDocumentEventValue = std::variant<
        bool,
        int64_t,
        double,
        std::string,
        std::vector<std::string>
    >;

    class ProjectDocumentEventDetails {
        std::unordered_map<std::string, ProjectDocumentEventValue> values_{};

    public:
        bool empty() const {
            return values_.empty();
        }

        bool has(std::string_view key) const {
            return values_.find(std::string(key)) != values_.end();
        }

        const ProjectDocumentEventValue* get(std::string_view key) const {
            auto it = values_.find(std::string(key));
            if (it == values_.end())
                return nullptr;
            return &it->second;
        }

        template<typename T>
        std::optional<T> getAs(std::string_view key) const {
            auto value = get(key);
            if (!value)
                return std::nullopt;
            auto typed = std::get_if<T>(value);
            if (!typed)
                return std::nullopt;
            return *typed;
        }

        ProjectDocumentEventDetails& set(std::string key, ProjectDocumentEventValue value) {
            values_[std::move(key)] = std::move(value);
            return *this;
        }

        ProjectDocumentEventDetails& remove(std::string_view key) {
            values_.erase(std::string(key));
            return *this;
        }

        std::vector<std::string> keys() const {
            std::vector<std::string> result;
            result.reserve(values_.size());
            for (const auto& [key, value] : values_) {
                (void) value;
                result.push_back(key);
            }
            return result;
        }
    };

    class ProjectDocumentEvent {
        ProjectDocumentEventKind kind_;
        std::string type_;
        ProjectRevision revision_{0};
        ProjectRevision previous_revision_{0};
        bool full_resync_recommended_{false};
        std::optional<ProjectObjectId> project_id_{};
        std::optional<ProjectObjectId> track_id_{};
        std::optional<ProjectObjectId> clip_id_{};
        std::optional<ProjectObjectId> audio_source_id_{};
        std::optional<int32_t> track_index_{};
        std::optional<int32_t> clip_numeric_id_{};
        ProjectDocumentEventDetails details_{};

    public:
        explicit ProjectDocumentEvent(ProjectDocumentEventKind kind, std::string type = {})
            : kind_(kind)
            , type_(std::move(type)) {
        }

        ProjectDocumentEventKind kind() const {
            return kind_;
        }

        std::string_view type() const {
            return type_;
        }

        ProjectRevision revision() const {
            return revision_;
        }

        ProjectRevision previousRevision() const {
            return previous_revision_;
        }

        bool fullResyncRecommended() const {
            return full_resync_recommended_;
        }

        const std::optional<ProjectObjectId>& projectId() const {
            return project_id_;
        }

        const std::optional<ProjectObjectId>& trackId() const {
            return track_id_;
        }

        const std::optional<ProjectObjectId>& clipId() const {
            return clip_id_;
        }

        const std::optional<ProjectObjectId>& audioSourceId() const {
            return audio_source_id_;
        }

        const std::optional<int32_t>& trackIndex() const {
            return track_index_;
        }

        const std::optional<int32_t>& clipNumericId() const {
            return clip_numeric_id_;
        }

        const ProjectDocumentEventDetails& details() const {
            return details_;
        }

        ProjectDocumentEventDetails& details() {
            return details_;
        }

        ProjectDocumentEvent& setType(std::string value) {
            type_ = std::move(value);
            return *this;
        }

        ProjectDocumentEvent& setRevision(ProjectRevision value) {
            revision_ = value;
            return *this;
        }

        ProjectDocumentEvent& setPreviousRevision(ProjectRevision value) {
            previous_revision_ = value;
            return *this;
        }

        ProjectDocumentEvent& setFullResyncRecommended(bool value) {
            full_resync_recommended_ = value;
            return *this;
        }

        ProjectDocumentEvent& setProjectId(ProjectObjectId value) {
            project_id_ = std::move(value);
            return *this;
        }

        ProjectDocumentEvent& setTrackId(ProjectObjectId value) {
            track_id_ = std::move(value);
            return *this;
        }

        ProjectDocumentEvent& setClipId(ProjectObjectId value) {
            clip_id_ = std::move(value);
            return *this;
        }

        ProjectDocumentEvent& setAudioSourceId(ProjectObjectId value) {
            audio_source_id_ = std::move(value);
            return *this;
        }

        ProjectDocumentEvent& setTrackIndex(int32_t value) {
            track_index_ = value;
            return *this;
        }

        ProjectDocumentEvent& setClipNumericId(int32_t value) {
            clip_numeric_id_ = value;
            return *this;
        }

        ProjectDocumentEvent& setDetail(std::string key, ProjectDocumentEventValue value) {
            details_.set(std::move(key), std::move(value));
            return *this;
        }
    };

    class ProjectDocumentEventListener {
    public:
        virtual ~ProjectDocumentEventListener() = default;

        virtual void projectLoaded(const ProjectDocumentEvent& event) { (void) event; }
        virtual void projectClosing(const ProjectDocumentEvent& event) { (void) event; }
        virtual void projectSaved(const ProjectDocumentEvent& event) { (void) event; }

        virtual void masterTrackChanged(const ProjectDocumentEvent& event) { (void) event; }

        virtual void trackAdded(const ProjectDocumentEvent& event) { (void) event; }
        virtual void trackRemoved(const ProjectDocumentEvent& event) { (void) event; }
        virtual void trackChanged(const ProjectDocumentEvent& event) { (void) event; }

        virtual void clipAdded(const ProjectDocumentEvent& event) { (void) event; }
        virtual void clipRemoved(const ProjectDocumentEvent& event) { (void) event; }
        virtual void clipChanged(const ProjectDocumentEvent& event) { (void) event; }

        virtual void audioSourceAdded(const ProjectDocumentEvent& event) { (void) event; }
        virtual void audioSourceRemoved(const ProjectDocumentEvent& event) { (void) event; }
        virtual void audioSourceChanged(const ProjectDocumentEvent& event) { (void) event; }

        virtual void pluginGraphChanged(const ProjectDocumentEvent& event) { (void) event; }
    };

    class ProjectDocumentEventSource {
    public:
        virtual ~ProjectDocumentEventSource() = default;

        virtual ProjectRevision currentRevision() const = 0;
        virtual ProjectDocumentEventListenerToken addProjectDocumentEventListener(ProjectDocumentEventListener& listener) = 0;
        virtual void removeProjectDocumentEventListener(ProjectDocumentEventListenerToken token) = 0;
    };

    class ProjectDocumentEventDispatcher : public ProjectDocumentEventSource {
        mutable std::mutex listeners_mutex_{};
        std::unordered_map<ProjectDocumentEventListenerToken, ProjectDocumentEventListener*> listeners_{};
        std::atomic<ProjectDocumentEventListenerToken> next_listener_token_{1};
        std::atomic<ProjectRevision> current_revision_{0};

        static void dispatchTo(ProjectDocumentEventListener& listener, const ProjectDocumentEvent& event) {
            switch (event.kind()) {
                case ProjectDocumentEventKind::ProjectLoaded:
                    listener.projectLoaded(event);
                    break;
                case ProjectDocumentEventKind::ProjectClosing:
                    listener.projectClosing(event);
                    break;
                case ProjectDocumentEventKind::ProjectSaved:
                    listener.projectSaved(event);
                    break;
                case ProjectDocumentEventKind::MasterTrackChanged:
                    listener.masterTrackChanged(event);
                    break;
                case ProjectDocumentEventKind::TrackAdded:
                    listener.trackAdded(event);
                    break;
                case ProjectDocumentEventKind::TrackRemoved:
                    listener.trackRemoved(event);
                    break;
                case ProjectDocumentEventKind::TrackChanged:
                    listener.trackChanged(event);
                    break;
                case ProjectDocumentEventKind::ClipAdded:
                    listener.clipAdded(event);
                    break;
                case ProjectDocumentEventKind::ClipRemoved:
                    listener.clipRemoved(event);
                    break;
                case ProjectDocumentEventKind::ClipChanged:
                    listener.clipChanged(event);
                    break;
                case ProjectDocumentEventKind::AudioSourceAdded:
                    listener.audioSourceAdded(event);
                    break;
                case ProjectDocumentEventKind::AudioSourceRemoved:
                    listener.audioSourceRemoved(event);
                    break;
                case ProjectDocumentEventKind::AudioSourceChanged:
                    listener.audioSourceChanged(event);
                    break;
                case ProjectDocumentEventKind::PluginGraphChanged:
                    listener.pluginGraphChanged(event);
                    break;
            }
        }

    public:
        ProjectRevision currentRevision() const override {
            return current_revision_.load(std::memory_order_acquire);
        }

        ProjectDocumentEventListenerToken addProjectDocumentEventListener(ProjectDocumentEventListener& listener) override {
            auto token = next_listener_token_.fetch_add(1, std::memory_order_acq_rel);
            std::lock_guard<std::mutex> lock(listeners_mutex_);
            listeners_[token] = &listener;
            return token;
        }

        void removeProjectDocumentEventListener(ProjectDocumentEventListenerToken token) override {
            std::lock_guard<std::mutex> lock(listeners_mutex_);
            listeners_.erase(token);
        }

        void setCurrentRevision(ProjectRevision revision) {
            current_revision_.store(revision, std::memory_order_release);
        }

        void emit(ProjectDocumentEvent event) {
            auto previousRevision = currentRevision();
            if (event.previousRevision() == 0)
                event.setPreviousRevision(previousRevision);
            if (event.revision() == 0)
                event.setRevision(previousRevision + 1);
            current_revision_.store(event.revision(), std::memory_order_release);

            std::vector<ProjectDocumentEventListener*> listeners;
            {
                std::lock_guard<std::mutex> lock(listeners_mutex_);
                listeners.reserve(listeners_.size());
                for (auto& [token, listener] : listeners_) {
                    (void) token;
                    if (listener)
                        listeners.push_back(listener);
                }
            }

            for (auto* listener : listeners)
                dispatchTo(*listener, event);
        }
    };

} // namespace uapmd
