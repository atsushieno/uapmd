#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"
#include "JsfxGfxScheduler.hpp"
#include "ysfx.h"

namespace uapmd_jsfx {

    // The editor of one JSFX effect, as a pixel buffer.
    //
    // ysfx draws `@gfx` straight into a BGRA buffer the host supplies, so there is no view
    // to create and nothing platform-specific in the path. What this adds is the part ysfx
    // leaves to the host: somewhere to draw, a thread to draw on, input to draw in
    // response to, and an answer when a script opens a menu.
    //
    // Drawing happens on a scheduler worker, never on the UI thread, because a script that
    // calls gfx_showmenu() waits inside `@gfx` for the user's choice. The wait is inherent
    // -- gfx_showmenu returns the chosen item, so something has to block -- and the only
    // question is which thread pays for it. Putting it on a worker is what keeps the
    // application's own UI responsive and its menu non-modal.
    class JsfxFramebufferUI : public uapmd_plugin_hosting::PluginFramebufferUIExtension,
                              public GfxTickable {
        ysfx_t* fx_;

        std::mutex frame_mutex_{};
        std::vector<uint8_t> front_{};   // handed to the host
        std::vector<uint8_t> back_{};    // drawn into
        uint32_t width_{0};
        uint32_t height_{0};
        uint64_t serial_{0};

        std::mutex state_mutex_{};
        uint32_t requested_width_{0};
        uint32_t requested_height_{0};
        double scale_factor_{1.0};
        bool size_dirty_{true};
        bool displayed_{false};
        bool running_{false};
        uapmd_plugin_hosting::FramebufferInput pending_input_{};
        std::vector<uapmd_plugin_hosting::FramebufferKeyEvent> pending_keys_{};

        uapmd_plugin_hosting::FramebufferUIHost* host_{nullptr};

        // The gfx_showmenu handshake. `menu_answer_` is written by whoever the host calls
        // back on; the worker waits on `menu_condition_` for it.
        std::mutex menu_mutex_{};
        std::condition_variable menu_condition_{};
        bool menu_pending_{false};
        bool menu_answered_{false};
        int32_t menu_answer_{0};
        uint64_t menu_generation_{0};

        static int32_t onShowMenu(void* userData, const char* spec, int32_t x, int32_t y);
        static void onSetCursor(void* userData, int32_t cursor);
        static const char* onGetDropFile(void* userData, int32_t index);

        std::string drop_file_returned_{};

    public:
        explicit JsfxFramebufferUI(ysfx_t* fx);
        ~JsfxFramebufferUI() override;

        // Starts and stops being driven by the shared scheduler.
        void start();
        void stop();

        uapmd_plugin_hosting::FramebufferPixelFormat pixelFormat() const override {
            return uapmd_plugin_hosting::FramebufferPixelFormat::BGRA8;
        }
        bool preferredSize(uint32_t& width, uint32_t& height) override;
        void surfaceSize(uint32_t width, uint32_t height, double scaleFactor) override;
        bool readFrame(const std::function<void(const uapmd_plugin_hosting::FramebufferView&)>& consume) override;
        void deliverInput(uapmd_plugin_hosting::FramebufferInput input) override;
        void uiHost(uapmd_plugin_hosting::FramebufferUIHost* host) override;
        void displayed(bool value) override;

        void tickGfx() override;
        uint32_t desiredFrameRate() override;

        // Releases a worker waiting on a menu, answering as if the user dismissed it.
        // Called when the host detaches and on the way down, because `ysfx_unload_code`
        // wants the same lock the waiting worker is holding: without this, destroying an
        // instance while a menu is open hangs whichever thread is destroying it.
        void abandonPendingMenu();
    };

}
