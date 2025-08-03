#include "PluginFormatCLAP.hpp"
#include <vector>

namespace remidy {

    PluginInstanceCLAP::PluginStatesCLAP::PluginStatesCLAP(PluginInstanceCLAP *owner)
        : owner(owner) {
        const auto plugin = owner->plugin;
        state_context_ext = (clap_plugin_state_context_t*) plugin->get_extension(plugin, CLAP_EXT_STATE_CONTEXT);
    }

    int64_t remidy_clap_stream_read(const struct clap_istream *stream, void *buffer, uint64_t size) {
        auto src = (std::vector<uint8_t>*) stream->ctx;
        auto actualSize = std::min((uint64_t) src->size(), size);
        memcpy(buffer, src->data(), actualSize);
        return static_cast<int64_t>(actualSize);
    }

    int64_t remidy_clap_stream_write(const struct clap_ostream *stream, const void *buffer, uint64_t size) {
        auto src = (std::vector<uint8_t>*) stream->ctx;
        auto actualSize = std::min((uint64_t) src->size(), size);
        memcpy(src->data(), buffer, actualSize);
        return static_cast<int64_t>(actualSize);
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

    void PluginInstanceCLAP::PluginStatesCLAP::getState(std::vector<uint8_t> &state, void *statePartId,
                                                        PluginStateSupport::StateContextType stateContextType,
                                                        bool includeUiState) {
        // Note that we cannot support `includeUiState = false` in CLAP...
        EventLoop::runTaskOnMainThread([&] {
            clap_ostream_t stream;
            stream.ctx = &state;
            stream.write = remidy_clap_stream_write;
            state_context_ext->save(owner->plugin, &stream, remidyContextTypeToCLAPContextType(stateContextType));
        });
    }

    void PluginInstanceCLAP::PluginStatesCLAP::setState(std::vector<uint8_t> &state, void *statePartId,
                                                        PluginStateSupport::StateContextType stateContextType,
                                                        bool includeUiState) {
        // Note that we cannot support `includeUiState = false` in CLAP...
        EventLoop::runTaskOnMainThread([&] {
            clap_istream_t stream;
            stream.ctx = &state;
            stream.read = remidy_clap_stream_read;
            state_context_ext->load(owner->plugin, &stream, remidyContextTypeToCLAPContextType(stateContextType));
        });
    }
}
