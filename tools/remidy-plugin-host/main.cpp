#include <remidy/remidy.hpp>
#include "remidy-webui/impl/WebViewProxySaucer.hpp"
#include "remidy-webui/impl/WebViewProxyChoc.hpp"
#include "remidy-webui/impl/EventLoopSaucer.hpp"
#include "remidy-webui/impl/SaucerWebEmbedded.hpp"
#include "AppModel.hpp"
#include "components/AudioDeviceSetup.hpp"
#include "components/AudioPluginSelectors.hpp"
#include "components/AudioPluginInstanceControl.hpp"
#include "components/AudioPlayerController.hpp"
#include <cpptrace/from_current.hpp>

std::unique_ptr<remidy::EventLoop> eventLoop{};

using namespace remidy::webui::saucer_wrapper;

int runMain(int argc, char** argv) {
    std::filesystem::path webDir{"web"};
#if 1
    SaucerWebEmbedded web{webDir};
    eventLoop = std::make_unique<EventLoopSaucer>(web.app());
    remidy::setEventLoop(eventLoop.get());
    remidy::webui::WebViewProxy::Configuration config{ .enableDebugger = true };
    WebViewProxySaucer proxy{config, web};
#else
    // choc does not register custom URI schemes in secure context, so its
    // JS web components don't work as expected. We will need its own web server.
    remidy::webui::WebViewProxy::Configuration config{ .enableDebugger = true };
    remidy::webui::choc_wrapper::WebViewProxyChoc proxy{config};
#endif

    remidy::EventLoop::initializeOnUIThread();

    // Register UI component callbacks to the WebView.
    uapmd::registerPluginViewEntryListFeatures(proxy);
    uapmd::registerAudioDeviceSetupFeatures(proxy);
    uapmd::registerPluginInstanceControlFeatures(proxy);
    uapmd::registerAudioPlayerManagerFeatures(proxy);

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

    return EXIT_SUCCESS;
}

int main(int argc, char** argv) {
    CPPTRACE_TRY {
        return runMain(argc, argv);
    } CPPTRACE_CATCH(const std::exception &e) {
        std::cerr << "Exception in remidy-plugin-host: " << e.what() << std::endl;
        cpptrace::from_current_exception().print();
        return EXIT_FAILURE;
    }
}