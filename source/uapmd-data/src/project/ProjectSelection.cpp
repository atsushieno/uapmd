#include "uapmd-data/uapmd-data.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <type_traits>

namespace uapmd {
    ProjectSelectionProjection projectSelection(const ProjectDocumentView& view, const ProjectSelection& selection) {
        ProjectSelectionProjection result;
        auto append = [](auto& values, const auto& value) {
            if (std::find(values.begin(), values.end(), value) == values.end())
                values.push_back(value);
        };
        for (const auto& id : selection.objects) {
            auto object = view.getObject(id);
            std::set<ProjectObjectId> visited;
            while (object && visited.insert(object->id).second) {
                if (object->kind == ProjectObjectKind::Track) {
                    append(result.track_ids, object->id);
                    break;
                }
                if (object->kind == ProjectObjectKind::Clip) {
                    append(result.clip_ids, object->id);
                    break;
                }
                object = object->parent_id ? view.getObject(*object->parent_id) : std::nullopt;
            }
        }
        auto includeTime = [&result](double start, double end) {
            if (!std::isfinite(start) || !std::isfinite(end) || start > end)
                return;
            if (!result.time_range)
                result.time_range = {start, end};
            else {
                result.time_range->first = std::min(result.time_range->first, start);
                result.time_range->second = std::max(result.time_range->second, end);
            }
        };
        for (const auto& region : selection.regions)
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, TimelineSelectionRegion>) {
                    if (value.track_ids && value.track_ids->empty())
                        return;
                    includeTime(value.start_seconds, value.end_seconds);
                    for (const auto& id : value.track_ids.value_or(view.trackIds()))
                        append(result.track_ids, id);
                } else {
                    const auto& clipId = [&]() -> const ProjectObjectId& {
                        if constexpr (std::is_same_v<T, MidiNoteSelectionRegion>)
                            return value.clip_id;
                        else
                            return value.lane.clip_id;
                    }();
                    if (auto clip = view.getClip(clipId)) {
                        append(result.clip_ids, clipId);
                        append(result.track_ids, clip->trackId);
                        auto start = view.clipContentTickToProjectSeconds(clipId, value.start_tick);
                        auto end = view.clipContentTickToProjectSeconds(clipId, value.end_tick);
                        if (start && end)
                            includeTime(*start, *end);
                    }
                }
            }, region);
        return result;
    }

    ProjectSelectionService::ProjectSelectionService(const ProjectDocumentView& view) : view_(view) {
        default_context_ = createContext();
        active_context_ = default_context_;
    }

    SelectionContextId ProjectSelectionService::createContext(std::optional<ProjectObjectId> owner) {
        const auto id = next_context_++;
        contexts_.emplace(id, Context{std::move(owner), {}});
        return id;
    }

    bool ProjectSelectionService::hasContext(SelectionContextId context) const {
        return contexts_.contains(context);
    }

    std::optional<ProjectSelection> ProjectSelectionService::snapshot(SelectionContextId context) const {
        auto it = contexts_.find(context);
        if (it == contexts_.end())
            return std::nullopt;
        return it->second.selection;
    }

    ProjectSelection ProjectSelectionService::activeSelection() const {
        return active_context_ ? snapshot(*active_context_).value_or(ProjectSelection{}) : ProjectSelection{};
    }

    bool ProjectSelectionService::validRegion(const ProjectSelectionRegion& region) const {
        return std::visit([this](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, TimelineSelectionRegion>) {
                if (!std::isfinite(value.start_seconds) || !std::isfinite(value.end_seconds) ||
                    value.start_seconds > value.end_seconds)
                    return false;
                if (value.track_ids)
                    for (const auto& id : *value.track_ids)
                        if (!view_.getTrack(id))
                            return false;
            } else {
                const auto& clipId = [&]() -> const ProjectObjectId& {
                    if constexpr (std::is_same_v<T, MidiNoteSelectionRegion>)
                        return value.clip_id;
                    else
                        return value.lane.clip_id;
                }();
                auto clip = view_.getClip(clipId);
                if (!clip || clip->clipType != ClipType::Midi || value.start_tick > value.end_tick)
                    return false;
                if constexpr (std::is_same_v<T, MidiNoteSelectionRegion>) {
                    if ((value.pitches && (value.pitches->first > value.pitches->second || value.pitches->second > 127)) ||
                        (value.group && *value.group > 15) || (value.channel && *value.channel > 15))
                        return false;
                } else {
                    if (value.lane.group > 15 || value.lane.channel > 15)
                        return false;
                    if (value.values && (!std::isfinite(value.values->first) || !std::isfinite(value.values->second) ||
                        value.values->first < 0 || value.values->second > 1 || value.values->first > value.values->second))
                        return false;
                    if (value.lane.note_id) {
                        auto note = view_.getObject(*value.lane.note_id);
                        if (!note || note->kind != ProjectObjectKind::Note || note->parent_id != clipId)
                            return false;
                    }
                }
            }
            return true;
        }, region);
    }

    SelectionUpdateResult ProjectSelectionService::replace(SelectionContextId context, ProjectSelection selection) {
        auto it = contexts_.find(context);
        if (it == contexts_.end())
            return {false, "Selection context no longer exists"};
        std::set<ProjectObjectId> seen;
        for (const auto& id : selection.objects)
            if (!view_.getObject(id))
                return {false, "Selection object no longer exists: " + id};
        std::erase_if(selection.objects, [&seen](const auto& id) { return !seen.insert(id).second; });
        for (const auto& region : selection.regions)
            if (!validRegion(region))
                return {false, "Invalid selection region"};
        if (it->second.selection == selection)
            return {true, {}};
        Notification event{Notification::Kind::Selection};
        event.context = context;
        event.previous = std::move(it->second.selection);
        event.current = selection;
        it->second.selection = std::move(selection);
        pending_.push_back(std::move(event));
        drain();
        return {true, {}};
    }

    void ProjectSelectionService::clear(SelectionContextId context) {
        replace(context, {});
    }

    bool ProjectSelectionService::setActiveContext(std::optional<SelectionContextId> context) {
        if (context && !hasContext(*context))
            return false;
        if (context == active_context_)
            return true;
        Notification event{Notification::Kind::Active};
        event.previous_active = active_context_;
        event.current_active = context;
        active_context_ = context;
        pending_.push_back(std::move(event));
        drain();
        return true;
    }

    void ProjectSelectionService::removeContext(SelectionContextId context) {
        if (context == default_context_ || !hasContext(context))
            return;
        beginDocumentBatch();
        clear(context);
        if (active_context_ == context)
            setActiveContext(default_context_);
        contexts_.erase(context);
        Notification event{Notification::Kind::Removed};
        event.context = context;
        pending_.push_back(std::move(event));
        endDocumentBatch();
    }

    SelectionListenerToken ProjectSelectionService::addListener(ProjectSelectionListener& listener) {
        const auto token = next_listener_++;
        listeners_[token] = &listener;
        return token;
    }

    void ProjectSelectionService::removeListener(SelectionListenerToken token) {
        listeners_.erase(token);
    }

    void ProjectSelectionService::beginDocumentBatch() { ++defer_depth_; }

    void ProjectSelectionService::endDocumentBatch() {
        if (defer_depth_ > 0)
            --defer_depth_;
        drain();
    }

    void ProjectSelectionService::reconcile() {
        beginDocumentBatch();
        std::vector<SelectionContextId> ids;
        for (const auto& [id, context] : contexts_)
            ids.push_back(id);
        for (auto id : ids) {
            auto& context = contexts_.at(id);
            if (context.owner && !view_.getObject(*context.owner)) {
                removeContext(id);
                continue;
            }
            auto selection = context.selection;
            std::erase_if(selection.objects, [this](const auto& object) { return !view_.getObject(object); });
            for (auto& region : selection.regions)
                if (auto* timeline = std::get_if<TimelineSelectionRegion>(&region); timeline && timeline->track_ids)
                    std::erase_if(*timeline->track_ids, [this](const auto& track) { return !view_.getTrack(track); });
            std::erase_if(selection.regions, [this](const auto& region) { return !validRegion(region); });
            replace(id, std::move(selection));
        }
        endDocumentBatch();
    }

    void ProjectSelectionService::reset() {
        beginDocumentBatch();
        std::vector<SelectionContextId> ids;
        for (const auto& [id, context] : contexts_)
            ids.push_back(id);
        for (auto id : ids)
            if (id == default_context_)
                clear(id);
            else
                removeContext(id);
        setActiveContext(default_context_);
        endDocumentBatch();
    }

    void ProjectSelectionService::drain() {
        if (defer_depth_ || notifying_)
            return;
        notifying_ = true;
        while (!pending_.empty()) {
            auto event = std::move(pending_.front());
            pending_.pop_front();
            const auto listeners = listeners_;
            for (const auto& [token, listener] : listeners) {
                if (!listeners_.contains(token))
                    continue;
                switch (event.kind) {
                    case Notification::Kind::Selection:
                        listener->selectionChanged(event.context, event.previous, event.current);
                        break;
                    case Notification::Kind::Active:
                        listener->activeContextChanged(event.previous_active, event.current_active);
                        break;
                    case Notification::Kind::Removed:
                        listener->selectionContextRemoved(event.context);
                        break;
                }
            }
        }
        notifying_ = false;
    }
}
