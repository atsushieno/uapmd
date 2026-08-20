#pragma once

#include <span>
#include <string_view>
#include <filesystem>
#include <memory>
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
