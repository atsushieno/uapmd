
#include <fstream>
#include <list>
#include <nlohmann/json.hpp>

#include "remidy.hpp"


template<typename T>
std::vector<T*> getUnownedList(std::vector<std::unique_ptr<T>>& list) {
    std::vector<T *> ret{};
    ret.reserve(list.size());
    for (auto& entry : list)
        ret.emplace_back(entry.get());
    return ret;
}
std::vector<remidy::PluginCatalogEntry*> remidy::PluginCatalog::getPlugins() {
    return getUnownedList(entries);
}
std::vector<remidy::PluginCatalogEntry*> remidy::PluginCatalog::getDenyList() {
    return getUnownedList(denyList);
}

void remidy::PluginCatalog::add(std::unique_ptr<PluginCatalogEntry> entry) {
    entries.emplace_back(std::move(entry));
}

void remidy::PluginCatalog::merge(PluginCatalog&& other) {
    for (auto& entry : other.entries)
        entries.emplace_back(std::move(entry));
}

void remidy::PluginCatalog::clear() {
    entries.clear();
}


std::vector<std::unique_ptr<remidy::PluginCatalogEntry>> fromJson(nlohmann::json j) {
    std::vector<std::unique_ptr<remidy::PluginCatalogEntry>> list{};
    auto jPlugins = j.at("plugins");
    for_each(jPlugins.begin(), jPlugins.end(), [&](nlohmann::ordered_json jPlugin) {
        auto entry = std::make_unique<remidy::PluginCatalogEntry>();
        std::string format = jPlugin.at("format");
        entry->format(format);
        std::string id = jPlugin.at("id");
        entry->pluginId(id);
        std::string bundle = jPlugin.at("bundle");
        entry->bundlePath(bundle);
        std::string name = jPlugin.at("name");
        entry->setMetadataProperty(remidy::PluginCatalogEntry::DisplayName, name);
        std::string vendor = jPlugin.at("vendor");
        entry->setMetadataProperty(remidy::PluginCatalogEntry::VendorName, vendor);
        std::string url = jPlugin.at("url");
        entry->setMetadataProperty(remidy::PluginCatalogEntry::ProductUrl, url);
        list.emplace_back(std::move(entry));
    });
    return list;
}

void remidy::PluginCatalog::load(std::filesystem::path& path) {
    if (!std::filesystem::exists(path))
        return;

    std::ifstream ifs{path.string()};
    nlohmann::ordered_json j;
    ifs >> j;
    ifs.close();

    for (auto& entry : fromJson(j))
        entries.emplace_back(std::move(entry));
}

auto pluginEntriesToJson(std::vector<remidy::PluginCatalogEntry*> list) {
    std::vector<nlohmann::ordered_json> ret{};
    for (auto e : list)
        ret.emplace_back(nlohmann::ordered_json {
            {"format", e->format()},
            {"id", e->pluginId()},
            {"bundle", e->bundlePath()},
            {"name", e->getMetadataProperty(remidy::PluginCatalogEntry::DisplayName)},
            {"vendor", e->getMetadataProperty(remidy::PluginCatalogEntry::VendorName)},
            {"url", e->getMetadataProperty(remidy::PluginCatalogEntry::ProductUrl)},
        });
    return ret;
}

nlohmann::json toJson(remidy::PluginCatalog* catalog) {
    auto plugins = pluginEntriesToJson(catalog->getPlugins());
    auto denyList = pluginEntriesToJson(catalog->getDenyList());
    nlohmann::json j = {
        {"plugins", std::vector(plugins.begin(), plugins.end()) },
        {"denyList", std::vector(denyList.begin(), denyList.end()) }
    };
    return j;
}

void remidy::PluginCatalog::save(std::filesystem::path& path) {
    if (!std::filesystem::exists(path.parent_path()))
        std::filesystem::create_directories(path.parent_path());

    nlohmann::ordered_json j = toJson(this);

    if (std::filesystem::exists(path))
        std::filesystem::remove(path);
    std::ofstream ofs{path.string()};
    ofs << j;
    ofs.close();
}

