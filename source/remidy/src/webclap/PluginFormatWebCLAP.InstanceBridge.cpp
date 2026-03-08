#include "PluginFormatWebCLAPInternal.hpp"

#if defined(__EMSCRIPTEN__)

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <emscripten.h>

#include "PluginInstanceWebCLAP.hpp"

namespace remidy::webclap {

    namespace {
        struct PendingInstantiation {
            PluginCatalogEntry* entry{};
            PluginFormat::PluginInstantiationOptions options{};
            InstantiateCallback callback;
        };

        std::atomic<int32_t> g_nextToken{1};
        std::mutex g_pendingMutex;
        std::unordered_map<int32_t, PendingInstantiation> g_pending;

        PendingInstantiation takePendingInstantiation(int32_t token) {
            std::scoped_lock lock(g_pendingMutex);
            auto it = g_pending.find(token);
            if (it == g_pending.end())
                return {};
            PendingInstantiation pending = std::move(it->second);
            g_pending.erase(it);
            return pending;
        }
    } // namespace

    int32_t enqueuePendingInstantiation(PluginCatalogEntry* entry,
                                        PluginFormat::PluginInstantiationOptions options,
                                        InstantiateCallback callback) {
        if (entry == nullptr || !callback)
            return 0;
        auto token = g_nextToken.fetch_add(1);
        std::scoped_lock lock(g_pendingMutex);
        g_pending.emplace(token, PendingInstantiation{entry, options, std::move(callback)});
        return token;
    }

    void failPendingInstantiation(int32_t token, const std::string& errorMessage) {
        auto pending = takePendingInstantiation(token);
        if (pending.callback)
            pending.callback(nullptr, errorMessage);
    }

    bool requestInstanceFromJs(int32_t token, const std::string& bundleUrl, const std::string& pluginId) {
        int available = EM_ASM_INT({
            return (globalThis.uapmdWebClap && typeof globalThis.uapmdWebClap.requestInstance === 'function') ? 1 : 0;
        });
        if (!available)
            return false;
        EM_ASM({
            globalThis.uapmdWebClap.requestInstance($0, UTF8ToString($1), UTF8ToString($2));
        }, token, bundleUrl.c_str(), pluginId.c_str());
        return true;
    }

    extern "C" EMSCRIPTEN_KEEPALIVE void uapmd_webclap_instance_ready(int32_t token,
                                                                      int32_t success,
                                                                      uint32_t instancePtr,
                                                                      const char* payload) {
        auto pending = takePendingInstantiation(token);
        if (!pending.entry || !pending.callback)
            return;

        auto payloadStr = payload ? std::string{payload} : std::string{};
        try {
            if (!success) {
                auto error = payloadStr.empty() ? std::string{"WebCLAP instantiation failed"} : payloadStr;
                pending.callback(nullptr, error);
                return;
            }

            if (instancePtr == 0) {
                pending.callback(nullptr, "WebCLAP runtime returned an invalid instance handle.");
                return;
            }

            auto rawInstance = reinterpret_cast<remidy::WebclapSdkInstance*>(static_cast<uintptr_t>(instancePtr));
            std::unique_ptr<remidy::WebclapSdkInstance> sdkHandle(rawInstance);
            auto instance = std::make_unique<remidy::PluginInstanceWebCLAP>(pending.entry,
                                                                            std::move(sdkHandle),
                                                                            payloadStr);
            std::unique_ptr<remidy::PluginInstance> baseInstance = std::move(instance);
            pending.callback(std::move(baseInstance), "");
        } catch (const std::exception& e) {
            Logger::global()->logError("[WebCLAP] instance_ready threw: %s", e.what());
            pending.callback(nullptr, e.what());
        } catch (...) {
            Logger::global()->logError("[WebCLAP] instance_ready threw an unknown exception.");
            pending.callback(nullptr, "WebCLAP instantiation threw an unknown exception.");
        }
    }

} // namespace remidy::webclap

#endif
