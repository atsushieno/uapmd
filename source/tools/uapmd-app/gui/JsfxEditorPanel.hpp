#pragma once

#if UAPMD_HAS_JSFX

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "imgui.h"
#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"

namespace uapmd_app {

    // Draws a plugin whose editor is a pixel buffer rather than a native view.
    //
    // The plugin renders on its own thread into a buffer we read once per frame and upload
    // as a texture. Input goes the other way: whatever Dear ImGui reports for the image is
    // handed back to the plugin, translated into the key codes it expects.
    //
    // This also services the plugin's menu requests. A script that calls gfx_showmenu()
    // waits on the plugin's own worker thread for an answer, so this must not block: it
    // records the request, opens a popup on the next frame, and completes the request when
    // the user chooses or dismisses it.
    class JsfxEditorPanel : public uapmd_plugin_hosting::FramebufferUIHost {
        int32_t instance_id_;
        std::string title_;
        uapmd_plugin_hosting::PluginFramebufferUIExtension* ui_;

        ImTextureData* texture_{nullptr};
        uint64_t uploaded_serial_{0};
        bool open_{true};
        bool pointer_was_over_{false};

        // Menu requests arrive on the plugin's thread and are serviced on ours.
        std::mutex menu_mutex_{};
        std::string menu_spec_{};
        std::function<void(int32_t)> menu_completion_{};
        bool menu_requested_{false};
        bool menu_open_{false};

        void ensureTexture(uint32_t width, uint32_t height);
        void releaseTexture();
        void deliverInputFrom(const ImVec2& imageOrigin, uint32_t width, uint32_t height);
        void renderPendingMenu();

    public:
        JsfxEditorPanel(int32_t instanceId, std::string title,
                        uapmd_plugin_hosting::PluginFramebufferUIExtension* ui);
        ~JsfxEditorPanel() override;

        int32_t instanceId() const { return instance_id_; }
        bool isOpen() const { return open_; }

        // Draws the editor window. Returns false once the user has closed it, after which
        // the caller should drop this panel.
        bool render();

        // Drops the link to the plugin without touching it, for when the plugin is gone
        // already. Rendering stops and the destructor no longer calls back into it.
        void forgetPlugin() { ui_ = nullptr; }

        // Frees textures whose renderer resources have now been released. Call it once per
        // frame, whether or not any editor is open: a panel that closes leaves its texture
        // behind for the renderer to finish with.
        static void collectRetiredTextures();

        void requestMenu(const std::string& spec, int32_t x, int32_t y,
                         std::function<void(int32_t)> completed) override;
        void setCursor(int32_t cursor) override;
        std::string droppedFile(int32_t index) override;
    };

}

#endif
