#include "Augene2Compiler.hpp"
#include <uapmd-augene2/uapmd-augene2.hpp>

#include <array>
#include <chrono>
#include <future>
#include <map>
#include <set>
#include <thread>

#include <choc/text/choc_JSON.h>
#include <imgui.h>
#include <uapmd-addin-core/uapmd-addin-core.hpp>
#include <uapmd-app-model/uapmd-app-model.hpp>

namespace uapmd_augene2 {
namespace {
using namespace uapmd;
constexpr auto extension_id = "augene2";

struct Binding {
    std::string track;
    std::string clip;
    bool operator==(const Binding&) const = default;
};

struct State {
    std::vector<Source> sources;
    std::map<std::string, Binding> bindings;
    bool automatic{false};
    bool operator==(const State&) const = default;
};

// Commands own this value store, never a pointer into an unloadable panel.
struct Store {
    State state;
    uint64_t generation{};
};

class SetState final : public ProjectCommand {
    std::shared_ptr<Store> store;
    State value;
public:
    SetState(std::shared_ptr<Store> target, State state) : store(std::move(target)), value(std::move(state)) {}
    std::string_view commandId() const override { return "augene2.setState"; }
    std::string description() const override { return "Update MML resources"; }
    size_t retainedSizeInBytes() const override {
        size_t size = sizeof(*this);
        for (const auto& source : value.sources)
            size += sizeof(Source) + source.path.size() + source.text.size() + source.external_path.size();
        for (const auto& [key, binding] : value.bindings)
            size += key.size() + binding.track.size() + binding.clip.size() + sizeof(Binding);
        return size;
    }
    void execute(ProjectCommandContext& context, ProjectCommandCompletion completion) override {
        if (store->state != value) {
            context.recordRevert(std::make_shared<SetState>(store, store->state));
            store->state = value;
            ++store->generation;
        }
        completion(ProjectCommandResult::success());
    }
};

bool safePath(const std::string& name) {
    const std::filesystem::path path(name);
    if (name.empty() || path.is_absolute() || path.has_root_name())
        return false;
    for (const auto& part : path)
        if (part == ".." || part == ".")
            return false;
    return true;
}

choc::value::Value manifest(const State& state) {
    auto root = choc::value::createObject("Augene2");
    root.addMember("version", 1);
    root.addMember("automatic", state.automatic);
    auto sources = choc::value::createEmptyArray();
    for (const auto& source : state.sources) {
        auto entry = choc::value::createObject("Source");
        entry.addMember("path", source.path);
        entry.addMember("external", source.external_path);
        entry.addMember("compile", source.compile);
        sources.addArrayElement(entry);
    }
    root.addMember("sources", sources);
    auto bindings = choc::value::createEmptyArray();
    for (const auto& [key, binding] : state.bindings) {
        auto entry = choc::value::createObject("Binding");
        entry.addMember("key", key);
        entry.addMember("track", binding.track);
        entry.addMember("clip", binding.clip);
        bindings.addArrayElement(entry);
    }
    root.addMember("bindings", bindings);
    return root;
}

class Integration final : public uapmd_addin::Panel,
                          public ProjectSerializationExtension,
                          public ProjectDocumentEventListener,
                          public std::enable_shared_from_this<Integration> {
    TimelineFacade& timeline;
    std::shared_ptr<Store> store{std::make_shared<Store>()};
    ProjectDocumentEventListenerToken listener{};
    std::future<Compilation> job;
    uint64_t job_generation{};
    uint64_t session{};
    bool open{false};
    bool picking{false};
    bool applying{false};
    bool manual_pending{false};
    std::optional<Compilation> ready;
    uint64_t ready_generation{};
    std::vector<Source> last_observed;
    std::string status;
    std::vector<std::string> diagnostics;
    std::array<char, 512> import_folder{};

    bool setState(State state) {
        auto result = timeline.commands().history().executeSynchronously(
            std::make_shared<SetState>(store, std::move(state)));
        if (!result.succeeded())
            status = result.error;
        return result.succeeded();
    }

    bool idleHistory() const {
        const auto history = timeline.commands().history().state();
        return !history.busy && !history.compoundOpen;
    }

    void startCompile() {
        if (job.valid() || applying)
            return;
        job_generation = store->generation;
        auto sources = store->state.sources;
        // A packaged_task future does not join a compiler thread when a panel
        // or project closes. The worker captures values only; stale results
        // are discarded by generation on the model thread.
        std::packaged_task<Compilation()> task([sources = std::move(sources)]() mutable {
            return compileSources(std::move(sources), true);
        });
        job = task.get_future();
        std::thread(std::move(task)).detach();
        status = "Compiling...";
    }

    struct Application {
        State state;
        Compilation compilation;
        size_t next{};
        uint64_t session{};
    };

    void failApply(std::string error) {
        timeline.commands().history().cancelStep([self = shared_from_this(), error = std::move(error)](ProjectCommandResult rollback) {
            self->applying = false;
            self->status = error;
            if (!rollback.succeeded())
                self->status += " Rollback failed: " + rollback.error;
        });
    }

    void finishApply(const std::shared_ptr<Application>& application) {
        auto& state = application->state;
        std::set<std::string> current;
        for (const auto& generated : application->compilation.clips)
            current.insert(generated.key);
        for (auto& [key, binding] : state.bindings)
            if (!current.contains(key) && !binding.clip.empty()) {
                auto track = timeline.addresses().trackIndex(binding.track);
                auto clip = timeline.addresses().clipId({binding.track, binding.clip});
                if (clip >= 0 && !timeline.removeClipFromTrack(track, clip)) {
                    failApply("Could not remove an obsolete MML clip.");
                    return;
                }
                binding.clip.clear();
            }
        if (!setState(std::move(state))) {
            failApply("Could not record MML bindings.");
            return;
        }
        timeline.commands().history().endStep([self = shared_from_this()](ProjectCommandResult result) {
            self->applying = false;
            self->status = result.succeeded() ? "Compilation applied." : result.error;
        });
    }

    void applyNext(const std::shared_ptr<Application>& application) {
        if (application->session != session) {
            failApply("Project changed during compilation.");
            return;
        }
        if (application->next == application->compilation.clips.size()) {
            finishApply(application);
            return;
        }
        const auto& generated = application->compilation.clips[application->next];
        auto& binding = application->state.bindings[generated.key];
        int32_t track = generated.master ? kMasterTrackIndex : timeline.addresses().trackIndex(binding.track);
        if (!generated.master && track < 0) {
            const auto prefix = generated.key.substr(0, generated.key.find('/')) + "/";
            for (const auto& [key, candidate] : application->state.bindings)
                if (key.starts_with(prefix)) {
                    auto existing = timeline.addresses().trackIndex(candidate.track);
                    if (existing >= 0) {
                        track = existing;
                        break;
                    }
                }
        }
        if (!generated.master && track < 0) {
            timeline.addEmptyTrack(ProjectMutationOrigin::User,
                [self = shared_from_this(), application, key = generated.key](int32_t index, std::string error) {
                    if (index < 0) {
                        self->failApply(error);
                        return;
                    }
                    auto id = self->timeline.addresses().trackReferenceId(index);
                    if (!id) {
                        self->failApply("New MML track has no document identity.");
                        return;
                    }
                    application->state.bindings[key].track = *id;
                    self->applyNext(application);
                });
            return;
        }
        auto track_id = timeline.addresses().trackReferenceId(track);
        if (!track_id) {
            failApply("MML destination track is unavailable.");
            return;
        }
        binding.track = *track_id;
        auto clip = timeline.addresses().clipId({binding.track, binding.clip});
        const auto& data = generated.content;
        const double beats = static_cast<double>(generated.position_ticks) / data.tick_resolution;
        const double seconds = timeline.masterTempoMap().beatsToSeconds(beats);
        auto position = TimelinePosition::fromSeconds(seconds,
            static_cast<int32_t>(timeline.masterTimelineTrack()->sampleRate()));
        if (clip >= 0) {
            if (!timeline.replaceMidiClipData(track, clip, data) ||
                !timeline.commands().setClipAnchor(track, clip, TimeReference::fromContainerStart({}, seconds)) ||
                !timeline.commands().setClipName(track, clip, generated.name)) {
                failApply("Could not update MML clip " + generated.name);
                return;
            }
        } else {
            auto added = generated.master
                ? timeline.addMasterMidiClip(position, data.ump_data, data.ump_tick_timestamps,
                    data.tick_resolution, data.tempo, data.tempo_changes, data.time_signature_changes,
                    generated.name, true)
                : timeline.addMidiClipToTrack(track, position, data.ump_data, data.ump_tick_timestamps,
                    data.tick_resolution, data.tempo, data.tempo_changes, data.time_signature_changes,
                    generated.name, false, true);
            if (!added.success) {
                failApply(added.error);
                return;
            }
            auto address = timeline.addresses().clipAddress(track, added.clipId);
            if (!address) {
                failApply("New MML clip has no document identity.");
                return;
            }
            binding.clip = address->clipReferenceId;
        }
        ++application->next;
        applyNext(application);
    }

    void apply(Compilation compilation) {
        auto result = timeline.commands().history().beginStep("Compile MML");
        if (!result.succeeded()) {
            status = result.error;
            return;
        }
        applying = true;
        auto application = std::make_shared<Application>();
        application->state = store->state;
        application->compilation = std::move(compilation);
        application->session = session;
        applyNext(application);
    }

    void importDocument(DocumentHandle handle, bool compile, std::string destination,
                        std::string relink, uint64_t picked_session,
                        std::shared_ptr<std::vector<DocumentHandle>> remaining = {}) {
        auto* provider = uapmd_app::AppModel::instance().documentProvider();
        const auto weak = weak_from_this();
        provider->readDocument(handle, [weak, handle, compile, destination, relink, picked_session, remaining]
            (DocumentIOResult result, std::vector<uint8_t> bytes) {
            auto self = weak.lock();
            if (!self || self->session != picked_session)
                return;
            if (!result.success) {
                self->status = result.error;
                self->picking = false;
                return;
            }
            auto* provider = uapmd_app::AppModel::instance().documentProvider();
            auto finish = [weak, handle, compile, destination, relink, picked_session, remaining,
                           text = std::string(bytes.begin(), bytes.end())](std::string external) {
                auto self = weak.lock();
                if (!self || self->session != picked_session)
                    return;
                self->picking = false;
                if (!self->idleHistory()) {
                    self->status = "Project is busy; import the file again when the current edit finishes.";
                    return;
                }
                State state = self->store->state;
                if (!relink.empty()) {
                    auto source = std::ranges::find(state.sources, relink, &Source::path);
                    if (source == state.sources.end())
                        return;
                    source->external_path = std::move(external);
                    source->text = text;
                } else {
                    // Desktop paths preserve the complete directory hierarchy,
                    // so separately imported siblings and ../ includes agree.
                    auto relative = external.empty() ? std::filesystem::path(handle.display_name)
                        : std::filesystem::path(external).relative_path();
                    auto path = (std::filesystem::path("sources") / destination / relative).lexically_normal().generic_string();
                    if (!safePath(path)) {
                        self->status = "Choose a project-relative resource folder.";
                        return;
                    }
                    auto source = std::ranges::find(state.sources, path, &Source::path);
                    if (source != state.sources.end()) {
                        if (source->external_path != external) {
                            self->status = "A different resource already uses that path.";
                            return;
                        }
                        source->compile = source->compile || compile;
                        source->text = text;
                    } else
                        state.sources.push_back({path, text, std::move(external), compile});
                }
                if (self->setState(std::move(state))) {
                    self->last_observed.clear();
                    self->status = "MML resources imported.";
                    if (remaining && !remaining->empty()) {
                        auto next = std::move(remaining->back());
                        remaining->pop_back();
                        self->picking = true;
                        self->importDocument(std::move(next), compile, destination, relink, picked_session, remaining);
                    }
                }
            };
            if (provider->folderPathsAreUsable())
                provider->resolveToPath(handle, [finish](DocumentIOResult resolved, std::filesystem::path path) {
                    finish(resolved.success ? path.string() : std::string{});
                });
            else
                finish({});
        });
    }

    void pick(bool compile, std::string relink = {}) {
        auto* provider = uapmd_app::AppModel::instance().documentProvider();
        if (!provider)
            return;
        picking = true;
        auto weak = weak_from_this();
        const auto picked_session = session;
        const auto destination = std::string(import_folder.data());
        provider->pickOpenDocuments({{"MML sources", {"text/plain", "application/octet-stream"}, {"*.mml", "*.mugene"}},
                                    {"All files", {"*/*"}, {"*"}}}, compile && relink.empty(),
            [weak, compile, destination, relink, picked_session](DocumentPickResult picked) {
                if (auto self = weak.lock(); self && self->session == picked_session) {
                    if (!picked.success || picked.handles.empty()) {
                        self->picking = false;
                        self->status = picked.error;
                        return;
                    }
                    // Read sequentially so all selected resources are registered
                    // in selection order, including on asynchronous providers.
                    auto remaining = std::make_shared<std::vector<DocumentHandle>>(std::move(picked.handles));
                    std::ranges::reverse(*remaining);
                    auto first = std::move(remaining->back());
                    remaining->pop_back();
                    self->importDocument(std::move(first), compile, destination, relink, picked_session, remaining);
                }
            });
    }

public:
    explicit Integration(TimelineFacade& facade) : timeline(facade) {
        listener = timeline.projectDocumentEvents().addProjectDocumentEventListener(*this);
        timeline.addProjectSerializationExtension(*this);
    }
    ~Integration() override {
        // Disabling only detaches the panel; the retained project service is
        // destroyed at application shutdown, before compiler resources vanish.
        if (job.valid())
            job.wait();
        timeline.removeProjectSerializationExtension(*this);
        timeline.projectDocumentEvents().removeProjectDocumentEventListener(listener);
    }
    void show() { open = true; }
    void deactivate() {
        ++session;
        ++store->generation;
        ready.reset();
        picking = false;
        manual_pending = false;
        open = false;
    }

    void update() override {
        if (job.valid() && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto result = job.get();
            if (job_generation == store->generation && !result.diagnostics.empty())
                diagnostics = result.diagnostics;
            if (job_generation == store->generation && result.attempted) {
                diagnostics = result.diagnostics;
                // Undo must not be immediately overwritten by an unchanged
                // external snapshot. Manual Compile always remains available.
                if (!result.success || result.sources != last_observed || manual_pending) {
                    last_observed = result.sources;
                    ready = std::move(result);
                    ready_generation = store->generation;
                }
            }
        }
        if (applying || picking || !idleHistory())
            return;
        if (ready) {
            if (ready_generation != store->generation) {
                ready.reset();
                return;
            }
            auto result = std::move(*ready);
            ready.reset();
            State refreshed = store->state;
            refreshed.sources = result.sources;
            if (refreshed != store->state && !setState(std::move(refreshed)))
                return;
            if (result.success)
                apply(std::move(result));
            else
                status = "Compilation failed; previous clips retained.";
            manual_pending = false;
        }
        if (!job.valid() && !applying && manual_pending)
            startCompile();
    }

    void render() override {
        if (!open)
            return;
        ImGui::SetNextWindowSize(ImVec2(720, 480), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Augene2 Integration", &open)) {
            ImGui::BeginDisabled(picking || applying || !idleHistory());
            if (ImGui::Button("Import MMLs..."))
                pick(true);
            ImGui::SameLine();
            if (ImGui::Button("Import included MML..."))
                pick(false);
            ImGui::SameLine();
            if (ImGui::Button("Compile"))
                manual_pending = true;
            // Preserve saved preferences, but leave synchronization inactive
            // until an event-based filesystem watcher is implemented.
            bool automatic = false;
            ImGui::BeginDisabled();
            ImGui::Checkbox("Enable Auto Synchronization", &automatic);
            ImGui::EndDisabled();
            ImGui::TextWrapped("Automatic synchronization is currently unavailable. Use Compile to refresh linked files.");
            ImGui::InputText("Resource folder (optional)", import_folder.data(), import_folder.size());
            ImGui::Separator();
            for (size_t i = 0; i < store->state.sources.size(); ++i) {
                const auto source = store->state.sources[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TextUnformatted(source.compile ? "Input" : "Include");
                ImGui::SameLine();
                ImGui::TextWrapped("%s", source.path.c_str());
                if (!source.external_path.empty())
                    ImGui::TextWrapped("Linked: %s", source.external_path.c_str());
                else
                    ImGui::TextUnformatted("Bundled copy");
                if (ImGui::SmallButton("Relink..."))
                    pick(source.compile, source.path);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) {
                    auto state = store->state;
                    state.sources.erase(state.sources.begin() + static_cast<std::ptrdiff_t>(i));
                    setState(std::move(state));
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            if (job.valid())
                ImGui::TextUnformatted("Checking / compiling sources...");
            ImGui::TextWrapped("%s", status.c_str());
            for (const auto& diagnostic : diagnostics)
                ImGui::TextWrapped("%s", diagnostic.c_str());
            if (!store->state.bindings.empty() && ImGui::CollapsingHeader("Track mapping"))
                for (const auto& [key, binding] : store->state.bindings) {
                    const auto index = timeline.addresses().trackIndex(binding.track);
                    const auto destination = index == kMasterTrackIndex ? "Master track"
                        : index >= 0 ? "Track " + std::to_string(index + 1) : "Track unavailable";
                    ImGui::Text("MML %s -> %s", key.c_str(), destination.c_str());
                }
        }
        ImGui::End();
    }

    std::string_view extensionId() const override { return extension_id; }
    bool saveProjectData(UapmdProjectData& project, std::string&) override {
        project.settings()[extension_id] = choc::json::toString(manifest(store->state));
        return true;
    }
    bool saveProjectExtensionData(ProjectSerializationWriteContext& context, std::string& error) override {
        for (const auto& source : store->state.sources)
            if (!context.writeExtensionFile(extension_id, source.path,
                    std::vector<uint8_t>(source.text.begin(), source.text.end()), error))
                return false;
        return true;
    }
    bool loadProjectData(UapmdProjectData& project, std::string& error) override {
        ++session;
        ++store->generation;
        store->state = {};
        ready.reset();
        last_observed.clear();
        manual_pending = false;
        const auto found = project.settings().find(extension_id);
        if (found == project.settings().end())
            return true;
        try {
            const auto value = choc::json::parse(found->second);
            if (value["version"].getWithDefault<int>(0) != 1)
                throw std::runtime_error("Unsupported Augene2 manifest version.");
            State loaded;
            loaded.automatic = value["automatic"].getWithDefault<bool>(false);
            std::set<std::string> paths;
            for (const auto entry : value["sources"]) {
                Source source;
                source.path = entry["path"].getString();
                if (!safePath(source.path) || !paths.insert(source.path).second)
                    throw std::runtime_error("Invalid or duplicate MML resource path.");
                source.external_path = entry["external"].getWithDefault<std::string>("");
                source.compile = entry["compile"].getWithDefault<bool>(false);
                loaded.sources.push_back(std::move(source));
            }
            for (const auto entry : value["bindings"])
                loaded.bindings.emplace(entry["key"].getString(), Binding{
                    std::string(entry["track"].getString()), std::string(entry["clip"].getString())});
            store->state = std::move(loaded);
            return true;
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
    }
    bool loadProjectExtensionData(ProjectSerializationReadContext& context, std::string& error) override {
        for (auto& source : store->state.sources) {
            auto bytes = context.readExtensionFile(extension_id, source.path, error);
            if (!bytes)
                return false;
            source.text.assign(bytes->begin(), bytes->end());
        }
        return true;
    }
    void projectClosing(const ProjectDocumentEvent&) override {
        ++session;
        ++store->generation;
        store->state = {};
        ready.reset();
        last_observed.clear();
        picking = false;
        manual_pending = false;
        status.clear();
        diagnostics.clear();
    }
};

std::weak_ptr<Integration> project_integration;

class Addin final : public uapmd_addin::Addin, public uapmd_addin::Command {
    std::shared_ptr<Integration> integration;
    uapmd_addin::CommandRegistry* commands{};
    uapmd_addin::PanelRegistry* panels{};
public:
    uapmd_addin::AddinIdentity identity() const noexcept override { return {"/uapmd/augene2", "integration"}; }
    std::string_view name() const noexcept override { return "Augene2 Integration"; }
    std::string_view path() const noexcept override { return "/uapmd/app/panel/v1"; }
    std::string_view id() const noexcept override { return "augene2.integration"; }
    std::string_view title() const noexcept override { return "Augene2 Integration"; }
    void invoke() noexcept override { if (integration) integration->show(); }
    bool initialize(uapmd_addin::AddinHost& host) noexcept override {
        try {
            auto* engine = static_cast<SequencerEngine*>(host.extensionPoint("/uapmd/engine/v1"));
            commands = static_cast<uapmd_addin::CommandRegistry*>(host.extensionPoint("/uapmd/app/command/v1"));
            panels = static_cast<uapmd_addin::PanelRegistry*>(host.extensionPoint(path()));
            if (!engine || !commands || !panels)
                return false;
            integration = project_integration.lock();
            if (!integration) {
                integration = std::make_shared<Integration>(engine->timeline());
                panels->retainPanel(integration);
                project_integration = integration;
            }
            commands->registerCommand(*this);
            panels->registerPanel(*integration);
            return true;
        } catch (...) {
            cleanup(host);
            return false;
        }
    }
    void cleanup(uapmd_addin::AddinHost&) noexcept override {
        if (commands)
            commands->unregisterCommand(*this);
        if (panels && integration)
            panels->unregisterPanel(*integration);
        if (integration)
            integration->deactivate();
        integration.reset();
        commands = nullptr;
        panels = nullptr;
    }
};

class Entry final : public uapmd_addin::AddinEntry {
    Addin addin;
    std::array<uapmd_addin::Addin*, 1> entries{&addin};
public:
    Entry() { uapmd_addin::registerBuiltinAddin(*this); }
    std::string_view packageId() const noexcept override { return "/uapmd/augene2"; }
    std::span<uapmd_addin::Addin* const> addins() noexcept override { return entries; }
};
Entry entry;

}

void registerProjectService(uapmd::TimelineFacade& timeline, uapmd_addin::PanelRegistry& panels) {
    auto integration = std::make_shared<Integration>(timeline);
    project_integration = integration;
    panels.retainPanel(std::move(integration));
}

}
