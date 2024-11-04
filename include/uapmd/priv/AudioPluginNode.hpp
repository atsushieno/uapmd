#pragma once

namespace uapmd {

    class AudioPluginNode {
        class Impl;
        Impl* impl;
    public:
        AudioPluginNode(const char* pluginId);
        virtual ~AudioPluginNode();

        const char* getPluginId() const;
        bool isBypassed();
        void setBypassed(bool value);
    };

}
