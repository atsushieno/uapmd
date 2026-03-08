#include "remidy/priv/plugin-format-webclap.hpp"

#if !defined(__EMSCRIPTEN__)

namespace remidy {}

#else

#include <choc/text/choc_JSON.h>
#include <filesystem>
#include <string>
#include <vector>

#include "PluginFormatWebCLAPInternal.hpp"

namespace remidy {

std::unique_ptr<PluginFormatWebCLAP> PluginFormatWebCLAP::create() {
    return std::make_unique<PluginFormatWebCLAP>();
}

PluginFormatWebCLAP::PluginFormatWebCLAP() : scanner_(*this) {}

std::string PluginFormatWebCLAP::name() {
    return "WebCLAP";
}

PluginUIThreadRequirement PluginFormatWebCLAP::requiresUIThreadOn(PluginCatalogEntry*) {
    return PluginUIThreadRequirement::None;
}

bool PluginFormatWebCLAP::canOmitUiState() {
    return false;
}

bool PluginFormatWebCLAP::isStateStructured() {
    return false;
}

PluginScanning* PluginFormatWebCLAP::scanning() {
    return &scanner_;
}

void PluginFormatWebCLAP::createInstance(PluginCatalogEntry* info,
                                         PluginInstantiationOptions options,
                                         std::function<void(std::unique_ptr<PluginInstance>, std::string)> callback) {
    if (info == nullptr) {
        callback(nullptr, "WebCLAP catalog entry is invalid.");
        return;
    }

    auto bundleUrl = info->bundlePath().string();
    if (bundleUrl.empty()) {
        callback(nullptr, "WebCLAP catalog entry is missing bundle URL metadata.");
        return;
    }
    auto pluginId = info->pluginId();
    if (pluginId.empty()) {
        callback(nullptr, "WebCLAP catalog entry is missing plugin ID metadata.");
        return;
    }

    auto token = webclap::enqueuePendingInstantiation(info, options, std::move(callback));
    if (token <= 0)
        return;

    if (!webclap::requestInstanceFromJs(token, bundleUrl, pluginId))
        webclap::failPendingInstantiation(token, "WebCLAP runtime bridge is unavailable in this build.");
}

PluginFormatWebCLAP::Scanner::Scanner(PluginFormatWebCLAP& ownerRef) : owner(ownerRef) {}

PluginScanning::ScanningStrategyValue PluginFormatWebCLAP::Scanner::scanRequiresLoadLibrary() {
    return ScanningStrategyValue::NEVER;
}

PluginScanning::ScanningStrategyValue PluginFormatWebCLAP::Scanner::scanRequiresInstantiation() {
    return ScanningStrategyValue::NEVER;
}

std::vector<std::unique_ptr<PluginCatalogEntry>> PluginFormatWebCLAP::Scanner::scanAllAvailablePlugins(bool) {
    std::vector<std::unique_ptr<PluginCatalogEntry>> entries;

    try {
        auto manifestPayload = webclap::resolveManifestPayload();
        auto manifest = choc::json::parse(manifestPayload);
        auto listedPlugins = manifest["plugins"];
        for (auto listed : listedPlugins) {
            auto entry = std::make_unique<PluginCatalogEntry>();
            std::string format = listed["format"].toString();
            entry->format(format);
            std::string id = listed["id"].toString();
            entry->pluginId(id);
            std::string name = listed["name"].toString();
            entry->displayName(name);
            std::string vendor = listed["vendor"].toString();
            entry->vendorName(vendor);
            std::string url = listed["url"].toString();
            entry->productUrl(url);
            std::string bundle = listed["bundle"].toString();
            entry->bundlePath(std::filesystem::path(bundle));
            entries.emplace_back(std::move(entry));
        }
    } catch (const std::exception& e) {
        auto message = std::string{"PluginFormatWebCLAP manifest parse failed: "} + e.what();
        Logger::global()->logWarning("%s", message.c_str());
    }

    return entries;
}

} // namespace remidy

#endif
