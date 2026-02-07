
#include <fstream>
#include <list>

#include <choc/text/choc_JSON.h>
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

bool remidy::PluginCatalog::contains(std::string& format, std::string& pluginId) const {
    for (auto & e : entries)
        if (e->format() == format && e->pluginId() == pluginId)
            return true;
    return false;
}

void remidy::PluginCatalog::add(std::unique_ptr<PluginCatalogEntry> entry) {
    entries.emplace_back(std::move(entry));
}

void remidy::PluginCatalog::merge(PluginCatalog&& other) {
    for (auto& entry : other.entries)
        entries.emplace_back(std::move(entry));
    for (auto& entry : other.denyList)
        denyList.emplace_back(std::move(entry));
}

void remidy::PluginCatalog::clear() {
    entries.clear();
}


std::vector<std::unique_ptr<remidy::PluginCatalogEntry>> fromJson(const choc::value::ValueView& j) {
    std::vector<std::unique_ptr<remidy::PluginCatalogEntry>> list{};
    auto jPlugins = j["plugins"];
    for (auto jPlugin : jPlugins) {
        auto entry = std::make_unique<remidy::PluginCatalogEntry>();
        std::string format = jPlugin["format"].toString();
        entry->format(format);
        std::string id = jPlugin["id"].toString();
        entry->pluginId(id);
        std::string bundle = jPlugin["bundle"].toString();
        entry->bundlePath(bundle);
        std::string name = jPlugin["name"].toString();
        entry->displayName(name);
        std::string vendor = jPlugin["vendor"].toString();
        entry->vendorName(vendor);
        std::string url = jPlugin["url"].toString();
        entry->productUrl(url);

        list.emplace_back(std::move(entry));
    }
    return list;
}

void remidy::PluginCatalog::load(std::filesystem::path& path) {
    if (!std::filesystem::exists(path))
        return;

    std::ostringstream ss;
    std::ifstream ifs{path.string()};
    ss << ifs.rdbuf();

    auto j = choc::json::parse(ss.str());

    for (auto& entry : fromJson(j.getView()))
        entries.emplace_back(std::move(entry));
}

auto pluginEntriesToJson(std::vector<remidy::PluginCatalogEntry*> list) {
    std::vector<choc::value::Value> ret{};
    for (auto e : list) {
        ret.emplace_back(choc::value::createObject("PluginCatalogEntry",
                                                   "format", std::string{e->format()},
                                                   "id", std::string{e->pluginId()},
                                                   "bundle", std::string{e->bundlePath().string()},
                                                   "name", std::string{e->displayName()},
                                                   "vendor", std::string{e->vendorName()},
                                                   "url", std::string{e->productUrl()})
        );
    }
    return ret;
}

choc::value::Value toJson(remidy::PluginCatalog* catalog) {
    auto plugins = pluginEntriesToJson(catalog->getPlugins());
    auto denyList = pluginEntriesToJson(catalog->getDenyList());
    auto j = choc::value::createObject("Catalog",
        "plugins", choc::value::createArray(plugins),
        "denyList", choc::value::createArray(denyList)
    );
    return j;
}

void remidy::PluginCatalog::save(std::filesystem::path& path) {
    if (!path.empty() && !std::filesystem::exists(path.parent_path()))
        std::filesystem::create_directories(path.parent_path());

    auto j = toJson(this);

    if (std::filesystem::exists(path))
        std::filesystem::remove(path);
    std::ofstream ofs{path.string()};
    // FIXME: this does not generate the expected string output, or I'm using this API in wrong way.
    ofs << choc::json::toString(j, true);
    ofs.close();
}

