#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <uapmd-addin-core/uapmd-addin-core.hpp>

namespace uapmd {

class SequencerEngine;

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

// Built-in addins must be registered before the AddinManager is constructed.
void registerBuiltinAddin(AddinEntry& entry);

class AddinManager {
public:
    explicit AddinManager(SequencerEngine& engine);
    ~AddinManager();

    AddinManager(const AddinManager&) = delete;
    AddinManager& operator=(const AddinManager&) = delete;

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
