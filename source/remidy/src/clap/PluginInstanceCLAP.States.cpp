#include "PluginFormatCLAP.hpp"
#include <algorithm>
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

    struct ClapStreamReadContext {
        std::vector<uint8_t> *buffer;
        size_t offset{0};
    };

    struct ClapStreamWriteContext {
        std::vector<uint8_t> *buffer;
        size_t offset{0};
    };

    int64_t remidy_clap_stream_read(const clap_istream_t *stream, void *buffer, uint64_t size) {
        auto *ctx = static_cast<ClapStreamReadContext *>(stream->ctx);
        if (!ctx || !ctx->buffer)
            return 0;
        if (ctx->offset >= ctx->buffer->size())
            return 0;
        const auto remaining = ctx->buffer->size() - ctx->offset;
        const auto bytesToCopy = static_cast<size_t>(std::min<uint64_t>(size, remaining));
        memcpy(buffer, ctx->buffer->data() + ctx->offset, bytesToCopy);
        ctx->offset += bytesToCopy;
        return static_cast<int64_t>(bytesToCopy);
    }

    int64_t remidy_clap_stream_write(const clap_ostream_t *stream, const void *buffer, uint64_t size) {
        auto *ctx = static_cast<ClapStreamWriteContext *>(stream->ctx);
        if (!ctx || !ctx->buffer)
            return 0;
        const auto requiredSize = ctx->offset + size;
        if (ctx->buffer->size() < requiredSize)
            ctx->buffer->resize(requiredSize);
        memcpy(ctx->buffer->data() + ctx->offset, buffer, size);
        ctx->offset += size;
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
            ClapStreamWriteContext context{&ret, 0};
            clap_ostream_t stream;
            stream.ctx = &context;
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
            ClapStreamReadContext context{&state, 0};
            clap_istream_t stream;
            stream.ctx = &context;
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
