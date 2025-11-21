#include "PluginFormatCLAP.hpp"
#include <vector>

namespace remidy {

    PluginInstanceCLAP::PluginStatesCLAP::PluginStatesCLAP(PluginInstanceCLAP *owner)
        : owner(owner) {
        if (!owner->plugin)
            return;
        EventLoop::runTaskOnMainThread([&] {
            const auto rawPlugin = owner->plugin->clapPlugin();
            state_context_ext = (clap_plugin_state_context_t*) rawPlugin->get_extension(rawPlugin, CLAP_EXT_STATE_CONTEXT);
            // Note: state_ext is handled by PluginProxy canUseState/stateSave/stateLoad
        });
    }

    int64_t remidy_clap_stream_read(const clap_istream_t *stream, void *buffer, uint64_t size) {
        auto src = (std::vector<uint8_t>*) stream->ctx;
        if (src->size() < size) {
            std::cerr << "remidy_clap_stream_read: truncated read" << std::endl;
            return 0;
        }
        memcpy(buffer, src->data(), size);
        // there is no way to read further anyway.
        return static_cast<int64_t>(size);
    }

    int64_t remidy_clap_stream_write(const clap_ostream_t *stream, const void *buffer, uint64_t size) {
        auto src = static_cast<std::vector<uint8_t> *>(stream->ctx);
        src->resize(size);
        memcpy(src->data(), buffer, size);
        return static_cast<int64_t>(size);
    }

    clap_plugin_state_context_type remidyContextTypeToCLAPContextType(PluginStateSupport::StateContextType src) {
        switch (src) {
            case PluginStateSupport::StateContextType::Remember:
            case PluginStateSupport::StateContextType::Copyable:
                return CLAP_STATE_CONTEXT_FOR_DUPLICATE;
            case PluginStateSupport::StateContextType::Preset:
                return CLAP_STATE_CONTEXT_FOR_PRESET;
            default:
                return CLAP_STATE_CONTEXT_FOR_PROJECT;
        }
    }

    std::vector<uint8_t> PluginInstanceCLAP::PluginStatesCLAP::getState(
            PluginStateSupport::StateContextType stateContextType, bool includeUiState) {
        std::vector<uint8_t> ret{};
        if (!owner->plugin)
            return ret;
        // Note that we cannot support `includeUiState = false` in CLAP...
        EventLoop::runTaskOnMainThread([&] {
            clap_ostream_t stream;
            stream.ctx = &ret;
            stream.write = remidy_clap_stream_write;
            if (state_context_ext) {
                const auto rawPlugin = owner->plugin->clapPlugin();
                state_context_ext->save(rawPlugin, &stream, remidyContextTypeToCLAPContextType(stateContextType));
            } else if (owner->plugin->canUseState()) {
                owner->plugin->stateSave(&stream);
            }
        });
        return ret;
    }

    void PluginInstanceCLAP::PluginStatesCLAP::setState(std::vector<uint8_t> &state,
                                                        PluginStateSupport::StateContextType stateContextType,
                                                        bool includeUiState) {
        if (!owner->plugin)
            return;
        // Note that we cannot support `includeUiState = false` in CLAP...
        EventLoop::runTaskOnMainThread([&] {
            clap_istream_t stream;
            stream.ctx = &state;
            stream.read = remidy_clap_stream_read;
            if (state_context_ext) {
                const auto rawPlugin = owner->plugin->clapPlugin();
                // should we check error and report it
                if (!state_context_ext->load(rawPlugin, &stream, remidyContextTypeToCLAPContextType(stateContextType)))
                    std::cerr << "PluginStatesCLAP::setState(): failed to load state (with context)" << std::endl;
            } else if (owner->plugin->canUseState()) {
                if (!owner->plugin->stateLoad(&stream))
                    std::cerr << "PluginStatesCLAP::setState(): failed to load state" << std::endl;
            }
        });
    }
}
