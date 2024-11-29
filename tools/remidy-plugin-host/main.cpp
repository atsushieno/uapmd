#include <saucer/smartview.hpp>
#include <remidy/remidy.hpp>
#include "impl/WebViewProxySaucer.hpp"
#include "impl/WebViewProxyChoc.hpp"
#include "impl/EventLoopSaucer.hpp"
#include "SaucerWebEmbedded.hpp"
#include "AudioDeviceSetup.hpp"
#include "AudioPluginSelectors.hpp"
#include "AppModel.hpp"

int main(int argc, char** argv) {
    std::filesystem::path webDir{"web"};
    std::string appTitle{"remidy-plugin-host"};
    SaucerWebEmbedded web{webDir, true};
    uapmd::WebViewProxy::Configuration config{ .enableDebugger = true };
    uapmd::WebViewProxySaucer proxy{config, web.webview()};
    EventLoopSaucer event_loop{web.app()};

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
