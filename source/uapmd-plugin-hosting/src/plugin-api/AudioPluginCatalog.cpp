#include "RemidyTypeConversion.hpp"

namespace uapmd_plugin_hosting {

    void AudioPluginCatalogEntry::bundlePath(const std::filesystem::path& value) {
        // Remote formats (WebCLAP) carry a URL here, which must not be normalized as a path.
        auto raw = value.generic_string();
        if (raw.starts_with("http://") || raw.starts_with("https://"))
            bundle_path_ = raw;
        else
            bundle_path_ = value.lexically_normal();
    }

    namespace {
        template<typename T>
        std::vector<T*> unownedList(std::vector<T>& list) {
            std::vector<T*> ret{};
            ret.reserve(list.size());
            for (auto& entry : list)
                ret.emplace_back(&entry);
            return ret;
        }
    }

    std::vector<AudioPluginCatalogEntry*> AudioPluginCatalog::getPlugins() {
        return unownedList(entries_);
    }

    std::vector<AudioPluginCatalogEntry*> AudioPluginCatalog::getDenyList() {
        return unownedList(deny_list_);
    }

    bool AudioPluginCatalog::contains(const std::string& format, const std::string& pluginId) const {
        for (auto& e : entries_)
            if (e.format() == format && e.pluginId() == pluginId)
                return true;
        return false;
    }

    void AudioPluginCatalog::add(AudioPluginCatalogEntry entry) {
        entries_.emplace_back(std::move(entry));
    }

    void AudioPluginCatalog::merge(AudioPluginCatalog&& other) {
        for (auto& entry : other.entries_)
            entries_.emplace_back(std::move(entry));
        for (auto& entry : other.deny_list_)
            deny_list_.emplace_back(std::move(entry));
    }

    void AudioPluginCatalog::clear() {
        entries_.clear();
    }

    // The plugin list cache is shared with the remidy scanner, so its serializer is reused
    // here instead of duplicating the JSON schema.
    void AudioPluginCatalog::load(const std::filesystem::path& path) {
        std::filesystem::path pathCopy{path};
        remidy::PluginCatalog source{};
        source.load(pathCopy);
        for (auto* e : source.getPlugins())
            entries_.emplace_back(toAudioPluginCatalogEntry(*e));
        for (auto* e : source.getDenyList())
            deny_list_.emplace_back(toAudioPluginCatalogEntry(*e));
    }

    void AudioPluginCatalog::save(const std::filesystem::path& path) {
        std::filesystem::path pathCopy{path};
        remidy::PluginCatalog target{};
        for (auto& e : entries_)
            target.add(toRemidyCatalogEntry(e));
        target.save(pathCopy);
    }

    AudioPluginCatalogEntry toAudioPluginCatalogEntry(const remidy::PluginCatalogEntry& source) {
        AudioPluginCatalogEntry ret{};
        ret.format(source.format());
        ret.pluginId(source.pluginId());
        ret.displayName(source.displayName());
        ret.vendorName(source.vendorName());
        ret.productUrl(source.productUrl());
        ret.bundlePath(source.bundlePath());
        return ret;
    }

    remidy::PluginCatalogEntry toRemidyCatalogEntry(const AudioPluginCatalogEntry& source) {
        remidy::PluginCatalogEntry ret{};
        auto format = source.format();
        ret.format(format);
        auto pluginId = source.pluginId();
        ret.pluginId(pluginId);
        ret.displayName(source.displayName());
        ret.vendorName(source.vendorName());
        ret.productUrl(source.productUrl());
        ret.bundlePath(source.bundlePath());
        return ret;
    }

}
