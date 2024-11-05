
#include "RemidyAudioPluginHostPAL.hpp"

void uapmd::RemidyAudioPluginHostPAL::createPluginInstance(std::string &format, std::string &pluginId, std::function<void(AudioPluginNode* node, std::string error)>&& callback) {

}

uapmd_status_t uapmd::RemidyAudioPluginHostPAL::processAudio(std::vector<remidy::AudioProcessContext *> contexts) {
    return 0;
}
