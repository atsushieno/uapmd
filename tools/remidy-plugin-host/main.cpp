#include <saucer/smartview.hpp>
#include <remidy/remidy.hpp>
#include "impl/WebViewProxySaucer.hpp"
#include "impl/WebViewProxyChoc.hpp"
#include "impl/EventLoopSaucer.hpp"
#include "impl/SaucerWebEmbedded.hpp"
#include "AudioDeviceSetup.hpp"
#include "AudioPluginSelectors.hpp"
#include "AppModel.hpp"

int main(int argc, char** argv) {
    std::filesystem::path webDir{"web"};
    std::string appTitle{"remidy-plugin-host"};
#if 1
    SaucerWebEmbedded web{webDir};
    EventLoopSaucer event_loop{web.app()};
    uapmd::WebViewProxy::Configuration config{ .enableDebugger = true };
    uapmd::WebViewProxySaucer proxy{config, web};
#else
    remidy::EventLoop& event_loop = *remidy::EventLoop::instance(); // default
    uapmd::WebViewProxy::Configuration config{ .enableDebugger = true };
    uapmd::WebViewProxyChoc proxy{config};
#endif

    remidy_tooling::PluginScanning scanning{};
    scanning.performPluginScanning();
    uapmd::AppModel::instance().pluginScanning = &scanning;

    remidy::EventLoop::instance(event_loop);

    remidy::EventLoop::initializeOnUIThread();

    // Register UI component callbacks to the WebView.
    uapmd::registerPluginViewEntryListFeatures(proxy);
    uapmd::registerAudioDeviceSetupFeatures(proxy);

    proxy.navigateToLocalFile("web/index.html");
    // Once it is loaded, fire "updated" events for device and plugin list so that
    //  the UI can reflect these target items correctly.
    proxy.evalJS("window.dispatchEvent(new Event('remidyDevicesUpdated'))");
    proxy.evalJS("window.dispatchEvent(new Event('remidyAudioPluginListUpdated'))");

    proxy.windowTitle(appTitle);
    proxy.show();

    remidy::EventLoop::start();

    return 0;
}
