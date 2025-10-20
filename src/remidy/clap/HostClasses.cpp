#include "PluginFormatCLAP.hpp"
#include "HostClasses.hpp"
#include <cstdint>

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

    bool RemidyCLAPHost::guiRequestShow() noexcept {
        return true;
    }

    bool RemidyCLAPHost::guiRequestHide() noexcept {
        return true;
    }

    bool RemidyCLAPHost::guiRequestResize(uint32_t width, uint32_t height) noexcept {
        auto* instance = attached_instance.load();
        if (!instance)
            return false;
        return instance->handleGuiResize(width, height);
    }

    void RemidyCLAPHost::guiClosed(bool wasDestroyed) noexcept {
        (void) wasDestroyed;
    }

    void RemidyCLAPHost::attachInstance(PluginInstanceCLAP *instance) noexcept {
        attached_instance.store(instance);
    }

    void RemidyCLAPHost::detachInstance(PluginInstanceCLAP *instance) noexcept {
        (void) instance;
        attached_instance.store(nullptr);
    }

}
