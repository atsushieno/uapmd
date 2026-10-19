#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ProjectDocumentEvents.hpp"

namespace uapmd {

    enum class ProjectObjectKind { Track, Clip, Note, AutomationPoint };

    struct ProjectObjectSnapshot {
        ProjectObjectId id;
        ProjectObjectKind kind;
        std::optional<ProjectObjectId> parent_id;
    };

    struct TimelineSelectionRegion {
        double start_seconds{0.0};
        double end_seconds{0.0};
        // nullopt means all tracks; an empty vector explicitly means none.
        std::optional<std::vector<ProjectObjectId>> track_ids;
        bool operator==(const TimelineSelectionRegion&) const = default;
    };

    struct MidiNoteSelectionRegion {
        ProjectObjectId clip_id;
        uint64_t start_tick{0};
        uint64_t end_tick{0};
        std::optional<std::pair<uint8_t, uint8_t>> pitches;
        std::optional<uint8_t> group;
        std::optional<uint8_t> channel;
        bool operator==(const MidiNoteSelectionRegion&) const = default;
    };

    struct MidiAutomationLaneAddress {
        ProjectObjectId clip_id;
        std::optional<ProjectObjectId> note_id;
        uint8_t group{0};
        uint8_t channel{0};
        // MIDI channel status (including registered/assignable per-note controllers).
        uint8_t status{0xB0};
        uint16_t parameter{0};
        bool operator==(const MidiAutomationLaneAddress&) const = default;
    };

    struct MidiAutomationSelectionRegion {
        MidiAutomationLaneAddress lane;
        uint64_t start_tick{0};
        uint64_t end_tick{0};
        std::optional<std::pair<double, double>> values;
        bool operator==(const MidiAutomationSelectionRegion&) const = default;
    };

    // Time intervals are half-open; equal endpoints represent a point selection.
    // Pitch and normalized-value constraints include both endpoints.
    using ProjectSelectionRegion = std::variant<
        TimelineSelectionRegion, MidiNoteSelectionRegion, MidiAutomationSelectionRegion>;

    struct ProjectSelection {
        // Ordered by relevance; the first object is primary.
        std::vector<ProjectObjectId> objects;
        std::vector<ProjectSelectionRegion> regions;
        bool operator==(const ProjectSelection&) const = default;
    };

    using SelectionContextId = uint64_t;
    using SelectionListenerToken = uint64_t;

    struct SelectionUpdateResult {
        bool success{false};
        std::string error;
        explicit operator bool() const { return success; }
    };

    class ProjectSelectionListener {
    public:
        virtual ~ProjectSelectionListener() = default;
        virtual void selectionChanged(SelectionContextId context,
            const ProjectSelection& previous, const ProjectSelection& current) {}
        virtual void activeContextChanged(std::optional<SelectionContextId> previous,
            std::optional<SelectionContextId> current) {}
        virtual void selectionContextRemoved(SelectionContextId context) {}
    };

    class ProjectDocumentView;

    // Consumer projection; the original object/region selection stays intact.
    struct ProjectSelectionProjection {
        std::vector<ProjectObjectId> clip_ids;
        std::vector<ProjectObjectId> track_ids;
        std::optional<std::pair<double, double>> time_range;
    };
    ProjectSelectionProjection projectSelection(const ProjectDocumentView& view, const ProjectSelection& selection);

    // Model-thread service. Never called from the audio thread. Notifications are
    // synchronous, except during a document batch or a reentrant callback.
    class ProjectSelectionService {
        struct Context {
            std::optional<ProjectObjectId> owner;
            ProjectSelection selection;
        };
        struct Notification {
            enum class Kind { Selection, Active, Removed } kind;
            SelectionContextId context{0};
            ProjectSelection previous;
            ProjectSelection current;
            std::optional<SelectionContextId> previous_active;
            std::optional<SelectionContextId> current_active;
        };
        const ProjectDocumentView& view_;
        std::map<SelectionContextId, Context> contexts_;
        std::map<SelectionListenerToken, ProjectSelectionListener*> listeners_;
        SelectionContextId next_context_{1};
        SelectionListenerToken next_listener_{1};
        SelectionContextId default_context_;
        std::optional<SelectionContextId> active_context_;
        std::deque<Notification> pending_;
        uint32_t defer_depth_{0};
        bool notifying_{false};

        void drain();
        bool validRegion(const ProjectSelectionRegion& region) const;

    public:
        explicit ProjectSelectionService(const ProjectDocumentView& view);
        ProjectSelectionService(const ProjectSelectionService&) = delete;
        ProjectSelectionService& operator=(const ProjectSelectionService&) = delete;

        SelectionContextId defaultContext() const { return default_context_; }
        SelectionContextId createContext(std::optional<ProjectObjectId> owner = {});
        bool hasContext(SelectionContextId context) const;
        void removeContext(SelectionContextId context);
        std::optional<ProjectSelection> snapshot(SelectionContextId context) const;
        ProjectSelection activeSelection() const;
        SelectionUpdateResult replace(SelectionContextId context, ProjectSelection selection);
        void clear(SelectionContextId context);
        bool setActiveContext(std::optional<SelectionContextId> context);
        std::optional<SelectionContextId> activeContext() const { return active_context_; }
        SelectionListenerToken addListener(ProjectSelectionListener& listener);
        void removeListener(SelectionListenerToken token);

        // Document owner brackets event delivery: reconcile final state before
        // document listeners run, then deliver selection notifications afterward.
        void beginDocumentBatch();
        void endDocumentBatch();
        void reconcile();
        void reset();
    };
}
