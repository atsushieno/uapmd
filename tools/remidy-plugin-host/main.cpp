#include <saucer/smartview.hpp>
#include <remidy/remidy.hpp>
#include "impl/WebViewProxySaucer.hpp"
#include "impl/WebViewProxyChoc.hpp"
#include "impl/EventLoopSaucer.hpp"
#include "impl/SaucerWebEmbedded.hpp"
#include "AppModel.hpp"
#include "AudioDeviceSetup.hpp"
#include "AudioPluginSelectors.hpp"
#include "AudioPluginInstanceControl.hpp"

int main(int argc, char** argv) {
    std::filesystem::path webDir{"web"};
#if 1
    SaucerWebEmbedded web{webDir};
    EventLoopSaucer event_loop{web.app()};
    uapmd::WebViewProxy::Configuration config{ .enableDebugger = true };
    uapmd::WebViewProxySaucer proxy{config, web};
    remidy::EventLoop::instance(event_loop);
#else
    uapmd::WebViewProxy::Configuration config{ .enableDebugger = true };
    uapmd::WebViewProxyChoc proxy{config};
#endif

    remidy_tooling::PluginScanning scanning{};
    scanning.performPluginScanning();
    uapmd::AppModel::instance().pluginScanning = &scanning;

    remidy::EventLoop::initializeOnUIThread();

    // Register UI component callbacks to the WebView.
    uapmd::registerPluginViewEntryListFeatures(proxy);
    uapmd::registerAudioDeviceSetupFeatures(proxy);
    uapmd::registerPluginInstanceControlFeatures(proxy);

    proxy.navigateToLocalFile("web/index.html");

    // Note that they are not ready yet even when a page is completely loaded (DOMContentLoaded).
    // For example, JS `eval` seem to be asynchronously executed in saucer (probably not just saucer).
    // Therefore, once it is loaded, fire "updated" events for device and plugin list so that
    //  the UI can reflect these target items correctly.
    proxy.evalJS("window.dispatchEvent(new Event('remidyDevicesUpdated'))");
    proxy.evalJS("window.dispatchEvent(new Event('remidyAudioPluginListUpdated'))");

    proxy.windowTitle("remidy-plugin-host");
    proxy.show();

    remidy::EventLoop::start();

    return 0;
}
