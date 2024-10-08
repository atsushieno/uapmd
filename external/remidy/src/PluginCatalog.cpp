
#include <fstream>
#include <list>
#include <nlohmann/json.hpp>
#include <travesty/base.h>

#include "remidy.hpp"
#include "utils.hpp"


std::vector<remidy::PluginCatalogEntry*> remidy::PluginCatalog::getPlugins() {
    std::vector<remidy::PluginCatalogEntry *> ret{};
    ret.reserve(list.size());
    for (auto& entry : list)
        ret.emplace_back(entry.get());
    return ret;
}

void remidy::PluginCatalog::add(std::unique_ptr<PluginCatalogEntry> entry) {
    list.emplace_back(std::move(entry));
}

void remidy::PluginCatalog::merge(PluginCatalog&& other) {
    for (auto& entry : other.list)
        list.emplace_back(std::move(entry));
}

void remidy::PluginCatalog::clear() {
    list.clear();
}


std::vector<std::unique_ptr<remidy::PluginCatalogEntry>> fromJson(nlohmann::json j) {
    std::vector<std::unique_ptr<remidy::PluginCatalogEntry>> list{};
    auto jPlugins = j.at("plugins");
    for_each(jPlugins.begin(), jPlugins.end(), [&](nlohmann::ordered_json jPlugin) {
        auto entry = std::make_unique<remidy::PluginCatalogEntry>();
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

void remidy::PluginCatalog::load(std::filesystem::path path) {
    if (!std::filesystem::exists(path))
        return;

    std::ifstream ifs{path.string()};
    nlohmann::ordered_json j;
    ifs >> j;
    ifs.close();

    for (auto& entry : fromJson(j))
        list.emplace_back(std::move(entry));
}

nlohmann::json toJson(remidy::PluginCatalog* catalog) {
    auto transformed = catalog->getPlugins() | std::views::transform([](remidy::PluginCatalogEntry* e) {
        return nlohmann::ordered_json {
            // FIXME: this "string" needs to be escaped.
            {"id", e->pluginId()},
            {"bundle", e->bundlePath()},
            {"name", e->getMetadataProperty(remidy::PluginCatalogEntry::DisplayName)},
            {"vendor", e->getMetadataProperty(remidy::PluginCatalogEntry::VendorName)},
            {"url", e->getMetadataProperty(remidy::PluginCatalogEntry::ProductUrl)},
        };
    });
    nlohmann::json j = {
        {"plugins", std::vector(transformed.begin(), transformed.end()) },
    };
    return j;
}

void remidy::PluginCatalog::save(std::filesystem::path path) {
    if (!std::filesystem::exists(path.parent_path()))
        std::filesystem::create_directories(path.parent_path());

    nlohmann::ordered_json j = toJson(this);

    if (std::filesystem::exists(path))
        std::filesystem::remove(path);
    std::ofstream ofs{path.string()};
    ofs << j;
    ofs.close();
}

