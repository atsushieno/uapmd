#include "HostClasses.hpp"
#include "PluginFormatCLAP.hpp"

namespace remidy {
    void RemidyCLAPHost::requestRestart() noexcept {
        // FIXME: implement
    }

    void RemidyCLAPHost::requestProcess() noexcept {
        // FIXME: implement
    }

    void RemidyCLAPHost::requestCallback() noexcept {
        // FIXME: implement
    }

    bool RemidyCLAPHost::threadCheckIsMainThread() const noexcept {
        return EventLoop::runningOnMainThread();
    }

    bool RemidyCLAPHost::threadCheckIsAudioThread() const noexcept {
        // FIXME: implement
        return true;
    }

}
