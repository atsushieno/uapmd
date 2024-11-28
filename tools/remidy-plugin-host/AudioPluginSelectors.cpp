
#include "AudioPluginSelectors.hpp"
#include "AppModel.hpp"

uapmd::AudioPluginViewEntryList uapmd::getPluginViewEntryList() {
    auto& catalog = uapmd::AppModel::instance().pluginScanning->catalog;
    AudioPluginViewEntryList ret{catalog};
    return ret;
}
