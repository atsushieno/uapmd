#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#if defined(UAPMD_BUILDING_ADDIN) && defined(_WIN32)
    #define UAPMD_ADDIN_EXPORT __declspec(dllexport)
#elif defined(UAPMD_BUILDING_ADDIN) && (defined(__GNUC__) || defined(__clang__))
    #define UAPMD_ADDIN_EXPORT __attribute__((visibility("default")))
#else
    #define UAPMD_ADDIN_EXPORT
#endif

namespace uapmd_addin {

class AddinHost;

struct AddinIdentity {
    std::string_view package_id;
    std::string_view addin_id;
};

class Addin {
public:
    virtual ~Addin() = default;

    virtual AddinIdentity identity() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;
    virtual std::string_view path() const noexcept = 0;
    virtual bool initialize(AddinHost& host) noexcept = 0;
    virtual void cleanup(AddinHost& host) noexcept = 0;
};

class AddinEntry {
public:
    virtual ~AddinEntry() = default;

    virtual std::string_view packageId() const noexcept = 0;
    virtual std::span<Addin* const> addins() noexcept = 0;
};

class AddinHost {
public:
    virtual ~AddinHost() = default;

    // The returned pointer is owned by the path-specific host implementation.
    // It is valid only while the corresponding extension point remains active.
    virtual void* extensionPoint(std::string_view path) noexcept = 0;
};

// Application commands contributed by addins.  The host owns the registry and
// decides where and how these commands are presented.  Addins only contribute
// an action descriptor and must unregister it during cleanup().
class Command {
public:
    virtual ~Command() = default;

    virtual std::string_view id() const noexcept = 0;
    virtual std::string_view title() const noexcept = 0;
    virtual int order() const noexcept { return 0; }
    virtual bool enabled() const noexcept { return true; }
    virtual void invoke() noexcept = 0;
};

class CommandRegistry {
public:
    void registerCommand(Command& command);
    void unregisterCommand(Command& command) noexcept;

    std::vector<Command*> commands() const;

private:
    std::vector<Command*> commands_;
};

// A clip the host is offering as the subject of a ClipCommand. The identifiers
// are the ones the host's own clip APIs take, so an addin resolves the clip
// through the engine rather than through anything carried here; the two flags
// are only enough for a command to say up front which clips it applies to.
struct ClipCommandTarget {
    int32_t track_index{-1};
    int32_t clip_id{-1};
    bool midi_clip{false};
    bool master_track{false};
};

// Commands scoped to one clip rather than to the application. The host offers
// these wherever it presents a clip, asking each command whether it applies to
// that particular clip first. Addins must unregister during cleanup().
class ClipCommand {
public:
    virtual ~ClipCommand() = default;

    virtual std::string_view id() const noexcept = 0;
    virtual std::string_view title() const noexcept = 0;
    virtual int order() const noexcept { return 0; }
    // False hides the command for this clip entirely; true but !enabled()
    // shows it greyed out.
    virtual bool appliesTo(const ClipCommandTarget& target) const noexcept = 0;
    virtual bool enabled(const ClipCommandTarget& target) const noexcept { (void) target; return true; }
    virtual void invoke(const ClipCommandTarget& target) noexcept = 0;
};

class ClipCommandRegistry {
public:
    void registerCommand(ClipCommand& command);
    void unregisterCommand(ClipCommand& command) noexcept;

    std::vector<ClipCommand*> commands() const;

private:
    std::vector<ClipCommand*> commands_;
};

// Timeline clip editors are contributed globally by addins, but instances are
// created and owned by the project/timeline host.  The service handles are
// intentionally opaque here: the application owns their concrete APIs and an
// addin must only use them according to the host contract for its build.
struct ClipEditorClip {
    int32_t track_index{-1};
    int32_t clip_id{-1};
    bool midi_clip{false};
    bool master_track{false};
};

struct ClipEditorContext {
    void* project{};
    void* timeline{};
    std::optional<ClipEditorClip> active_clip;
    void* selection_service{};
    void* undo{};
    void* transport{};
    void* clipboard{};
    void* command_router{};
};

class ClipEditor {
public:
    virtual ~ClipEditor() = default;

    virtual void update() noexcept = 0;
    virtual void render() noexcept = 0;
};

class ClipEditorAddin {
public:
    virtual ~ClipEditorAddin() = default;

    virtual std::string_view id() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;
    virtual bool supports(const ClipEditorContext& context) const noexcept = 0;
    virtual std::unique_ptr<ClipEditor> createEditor(const ClipEditorContext& context) = 0;
};

class ClipEditorRegistry {
public:
    void registerEditor(ClipEditorAddin& addin);
    void unregisterEditor(ClipEditorAddin& addin) noexcept;

    std::vector<ClipEditorAddin*> editors() const;

private:
    std::vector<ClipEditorAddin*> editors_;
};

// Owned by a project/timeline editor.  Rebuilding is explicit because changing
// project or active-clip context invalidates every editor instance.
class ClipEditorHost {
public:
    explicit ClipEditorHost(ClipEditorRegistry* registry = nullptr) noexcept;

    void setRegistry(ClipEditorRegistry* registry) noexcept;
    void setContext(ClipEditorContext context);
    void setActiveClip(std::optional<ClipEditorClip> clip);
    void rebuild();

    const ClipEditorContext& context() const noexcept { return context_; }
    const std::vector<std::unique_ptr<ClipEditor>>& editors() const noexcept { return editors_; }

    void update() noexcept;
    void render() noexcept;

private:
    ClipEditorRegistry* registry_{};
    ClipEditorContext context_{};
    std::vector<std::unique_ptr<ClipEditor>> editors_;
};

using AddinEntryFunction = AddinEntry* (*)() noexcept;

enum class AddinState {
    Inactive,
    Initializing,
    Active,
    CleaningUp,
    Failed,
};

struct AddinInfo {
    std::string package_id;
    std::string addin_id;
    std::string name;
    std::string path;
    std::filesystem::path library_path;
    bool built_in = false;
    AddinState state = AddinState::Inactive;
    std::string message;
};

// Built-in addins must be registered before initialize().
void registerBuiltinAddin(AddinEntry& entry);

class AddinManager {
public:
    AddinManager();
    ~AddinManager();

    AddinManager(const AddinManager&) = delete;
    AddinManager& operator=(const AddinManager&) = delete;

    void registerExtensionPoint(std::string_view path, void* extensionPoint);
    void initialize();
    bool setEnabled(const std::string& packageId, const std::string& addinId, bool enabled);
    void shutdown();

    const std::vector<std::filesystem::path>& addinDirectories() const noexcept;
    const std::vector<AddinInfo>& addins() const noexcept;
    const std::string& lastError() const noexcept;
    static bool supportsDynamicLoading() noexcept;

private:
    void loadInstalledAddins();

    class Impl;
    std::unique_ptr<Impl> impl_;
};

const char* addinStateName(AddinState state) noexcept;

} // namespace uapmd

// Every dynamically loaded addin library exports this symbol. Addin targets
// define UAPMD_BUILDING_ADDIN through add_uapmd_addin_library().
extern "C" UAPMD_ADDIN_EXPORT uapmd_addin::AddinEntry* uapmd_addin_entry() noexcept;
