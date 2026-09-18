#include "JsfxEditorPanel.hpp"

#if UAPMD_HAS_JSFX

#include <algorithm>
#include <cstring>
#include <vector>

#include "imgui_internal.h"

namespace uapmd_app {

    namespace {
        // JSFX key codes follow pugl's, which is what ysfx documents. Only the keys a
        // script can actually ask about are translated; ordinary characters pass through as
        // themselves.
        constexpr uint32_t kKeyBackspace = 0x08;
        constexpr uint32_t kKeyEscape = 0x1b;
        constexpr uint32_t kKeyDelete = 0x7f;
        constexpr uint32_t kKeyF1 = 0xe000;
        constexpr uint32_t kKeyLeft = kKeyF1 + 12;
        constexpr uint32_t kKeyUp = kKeyF1 + 13;
        constexpr uint32_t kKeyRight = kKeyF1 + 14;
        constexpr uint32_t kKeyDown = kKeyF1 + 15;
        constexpr uint32_t kKeyPageUp = kKeyF1 + 16;
        constexpr uint32_t kKeyPageDown = kKeyF1 + 17;
        constexpr uint32_t kKeyHome = kKeyF1 + 18;
        constexpr uint32_t kKeyEnd = kKeyF1 + 19;
        constexpr uint32_t kKeyInsert = kKeyF1 + 20;

        constexpr uint32_t kModShift = 1 << 0;
        constexpr uint32_t kModCtrl = 1 << 1;
        constexpr uint32_t kModAlt = 1 << 2;
        constexpr uint32_t kModSuper = 1 << 3;

        uint32_t currentModifiers() {
            const auto& io = ImGui::GetIO();
            uint32_t mods = 0;
            if (io.KeyShift) mods |= kModShift;
            if (io.KeyCtrl) mods |= kModCtrl;
            if (io.KeyAlt) mods |= kModAlt;
            if (io.KeySuper) mods |= kModSuper;
            return mods;
        }

        bool translateKey(ImGuiKey key, uint32_t& out) {
            switch (key) {
                case ImGuiKey_Backspace: out = kKeyBackspace; return true;
                case ImGuiKey_Escape: out = kKeyEscape; return true;
                case ImGuiKey_Delete: out = kKeyDelete; return true;
                case ImGuiKey_LeftArrow: out = kKeyLeft; return true;
                case ImGuiKey_RightArrow: out = kKeyRight; return true;
                case ImGuiKey_UpArrow: out = kKeyUp; return true;
                case ImGuiKey_DownArrow: out = kKeyDown; return true;
                case ImGuiKey_PageUp: out = kKeyPageUp; return true;
                case ImGuiKey_PageDown: out = kKeyPageDown; return true;
                case ImGuiKey_Home: out = kKeyHome; return true;
                case ImGuiKey_End: out = kKeyEnd; return true;
                case ImGuiKey_Insert: out = kKeyInsert; return true;
                case ImGuiKey_Enter: out = '\r'; return true;
                case ImGuiKey_Tab: out = '\t'; return true;
                case ImGuiKey_Space: out = ' '; return true;
                default: break;
            }
            if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
                out = static_cast<uint32_t>('a' + (key - ImGuiKey_A));
                return true;
            }
            if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
                out = static_cast<uint32_t>('0' + (key - ImGuiKey_0));
                return true;
            }
            if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) {
                out = kKeyF1 + static_cast<uint32_t>(key - ImGuiKey_F1);
                return true;
            }
            return false;
        }

        // JSFX menu descriptions are '|'-separated, with a leading '#' marking a disabled
        // item, '!' a checked one, '>' the start of a submenu and '<' its end. Only the
        // flat form is handled here; a nested menu is flattened rather than dropped, so a
        // script that uses one still works, if less prettily.
        struct MenuItem {
            std::string label;
            bool disabled{false};
            bool checked{false};
            bool separator{false};
            int32_t id{0};
        };

        std::vector<MenuItem> parseMenu(const std::string& spec) {
            std::vector<MenuItem> items{};
            int32_t nextId = 1;
            size_t start = 0;
            while (start <= spec.size()) {
                const size_t end = spec.find('|', start);
                std::string entry = spec.substr(start, end == std::string::npos ? std::string::npos : end - start);
                start = end == std::string::npos ? spec.size() + 1 : end + 1;

                MenuItem item{};
                size_t i = 0;
                bool submenuEnd = false;
                while (i < entry.size()) {
                    if (entry[i] == '#') { item.disabled = true; i++; }
                    else if (entry[i] == '!') { item.checked = true; i++; }
                    else if (entry[i] == '>') { i++; }
                    else if (entry[i] == '<') { submenuEnd = true; i++; }
                    else break;
                }
                item.label = entry.substr(i);
                (void) submenuEnd;
                if (item.label.empty())
                    item.separator = true;
                else
                    item.id = nextId;
                // Every entry consumes an id in JSFX's numbering, including ones that turn
                // out to be separators, so the ids the script gets back stay aligned.
                nextId++;
                items.emplace_back(std::move(item));
            }
            return items;
        }
    }

    JsfxEditorPanel::JsfxEditorPanel(int32_t instanceId, std::string title,
                                     uapmd_plugin_hosting::PluginFramebufferUIExtension* ui) :
            instance_id_(instanceId), title_(std::move(title)), ui_(ui) {
        if (ui_)
            ui_->uiHost(this);
    }

    JsfxEditorPanel::~JsfxEditorPanel() {
        if (ui_)
            ui_->uiHost(nullptr);
        // Whatever was waiting on a menu has been released by the detach above; completing
        // it here as well would be harmless but is not needed.
        releaseTexture();
    }

    void JsfxEditorPanel::ensureTexture(uint32_t width, uint32_t height) {
        if (texture_ && texture_->Width == static_cast<int>(width) &&
            texture_->Height == static_cast<int>(height))
            return;
        releaseTexture();
        texture_ = IM_NEW(ImTextureData)();
        texture_->Create(ImTextureFormat_RGBA32, static_cast<int>(width), static_cast<int>(height));
        texture_->SetStatus(ImTextureStatus_WantCreate);
        texture_->UseColors = true;
        // Registering is what gets the texture in front of the renderer, and it has to be
        // this rather than appending to ImGuiPlatformIO::Textures: that list is rebuilt from
        // the font atlases plus the registered user textures at the end of every frame, so
        // anything pushed into it directly survives at most one frame and then silently
        // stops being uploaded.
        ImGui::RegisterUserTexture(texture_);
        uploaded_serial_ = 0;
    }

    namespace {
        // Textures that have been given up but whose GPU resources the renderer has not
        // released yet. A backend only frees one when it sees ImTextureStatus_WantDestroy
        // while rendering, so deleting the object at that moment would leak the texture on
        // the GPU and leave the backend holding a dangling pointer.
        std::vector<ImTextureData*>& retiredTextures() {
            static std::vector<ImTextureData*> instance{};
            return instance;
        }
    }

    void JsfxEditorPanel::releaseTexture() {
        if (!texture_)
            return;
        // It stays registered for now: a renderer only frees the texture it made when it
        // sees ImTextureStatus_WantDestroy while rendering, so unregistering here would
        // take it out of sight and leak it. WantDestroyNextFrame also stops SetStatus()
        // from turning the eventual Destroyed back into WantCreate, which is what it does
        // for a texture that still has pixels.
        texture_->WantDestroyNextFrame = true;
        texture_->SetStatus(ImTextureStatus_WantDestroy);
        retiredTextures().emplace_back(texture_);
        texture_ = nullptr;
    }

    void JsfxEditorPanel::collectRetiredTextures() {
        auto& retired = retiredTextures();
        for (auto it = retired.begin(); it != retired.end();) {
            auto* tex = *it;
            if (tex->Status != ImTextureStatus_Destroyed) {
                ++it;
                continue;
            }
            ImGui::UnregisterUserTexture(tex);
            IM_DELETE(tex);
            it = retired.erase(it);
        }
    }

    void JsfxEditorPanel::deliverInputFrom(const ImVec2& imageOrigin, uint32_t width, uint32_t height) {
        auto& io = ImGui::GetIO();
        uapmd_plugin_hosting::FramebufferInput input{};

        const ImVec2 mouse = io.MousePos;
        const int32_t localX = static_cast<int32_t>(mouse.x - imageOrigin.x);
        const int32_t localY = static_cast<int32_t>(mouse.y - imageOrigin.y);
        input.pointerX = std::clamp(localX, 0, static_cast<int32_t>(width));
        input.pointerY = std::clamp(localY, 0, static_cast<int32_t>(height));

        const bool over = ImGui::IsItemHovered();
        input.pointerOver = over;
        input.hasFocus = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        input.visible = true;
        input.modifiers = currentModifiers();

        // Buttons follow JSFX's mouse_cap bits: 1 left, 2 right, 64 middle.
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) input.buttons |= 1;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) input.buttons |= 2;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) input.buttons |= 64;
        // Only report buttons while the pointer is over the editor, so that a drag started
        // elsewhere in the application does not reach the script.
        if (!over && !ImGui::IsItemActive())
            input.buttons = 0;

        if (over) {
            input.wheel = io.MouseWheel;
            input.horizontalWheel = io.MouseWheelH;
        }

        // Dear ImGui does not expose its key event queue, so transitions are polled. Both
        // edges are reported, because a script that tracks held keys needs the release as
        // much as the press.
        if (input.hasFocus) {
            for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++) {
                const auto imKey = static_cast<ImGuiKey>(key);
                const bool pressed = ImGui::IsKeyPressed(imKey, false);
                const bool released = ImGui::IsKeyReleased(imKey);
                if (!pressed && !released)
                    continue;
                uint32_t translated;
                if (!translateKey(imKey, translated))
                    continue;
                if (pressed)
                    input.keys.emplace_back(uapmd_plugin_hosting::FramebufferKeyEvent{
                            input.modifiers, translated, true});
                if (released)
                    input.keys.emplace_back(uapmd_plugin_hosting::FramebufferKeyEvent{
                            input.modifiers, translated, false});
            }
        }

        ui_->deliverInput(std::move(input));
    }

    void JsfxEditorPanel::renderPendingMenu() {
        std::string spec;
        bool shouldOpen = false;
        {
            std::lock_guard lock{menu_mutex_};
            if (menu_requested_ && !menu_open_) {
                spec = menu_spec_;
                menu_open_ = true;
                menu_requested_ = false;
                shouldOpen = true;
            }
        }

        const char* popupId = "##jsfx-script-menu";
        if (shouldOpen)
            ImGui::OpenPopup(popupId);

        bool stillOpen;
        {
            std::lock_guard lock{menu_mutex_};
            stillOpen = menu_open_;
        }
        if (!stillOpen)
            return;

        int32_t chosen = -1;
        bool closed = false;
        if (ImGui::BeginPopup(popupId)) {
            std::string currentSpec;
            {
                std::lock_guard lock{menu_mutex_};
                currentSpec = menu_spec_;
            }
            for (auto& item : parseMenu(currentSpec)) {
                if (item.separator) {
                    ImGui::Separator();
                    continue;
                }
                if (ImGui::MenuItem(item.label.c_str(), nullptr, item.checked, !item.disabled))
                    chosen = item.id;
            }
            ImGui::EndPopup();
        } else {
            // The popup is gone, which means it was dismissed without a choice.
            closed = true;
        }

        if (chosen < 0 && !closed)
            return;

        std::function<void(int32_t)> completion;
        {
            std::lock_guard lock{menu_mutex_};
            if (!menu_open_)
                return;
            menu_open_ = false;
            completion.swap(menu_completion_);
        }
        // Answering even a dismissal matters: the script is waiting inside gfx_showmenu
        // and a menu that is never answered parks its thread for good.
        if (completion)
            completion(chosen < 0 ? 0 : chosen);
        if (chosen >= 0)
            ImGui::CloseCurrentPopup();
    }

    bool JsfxEditorPanel::render() {
        if (!ui_ || !open_)
            return false;

        ImGui::SetNextWindowSize(ImVec2(520, 360), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title_.c_str(), &open_)) {
            ImGui::End();
            // Still tell the plugin nobody is looking, so it stops drawing.
            ui_->displayed(false);
            return open_;
        }
        ui_->displayed(true);

        const ImVec2 available = ImGui::GetContentRegionAvail();
        const uint32_t width = static_cast<uint32_t>(std::max(1.0f, available.x));
        const uint32_t height = static_cast<uint32_t>(std::max(1.0f, available.y));
        const float scale = ImGui::GetIO().DisplayFramebufferScale.x > 0.0f
                ? ImGui::GetIO().DisplayFramebufferScale.x : 1.0f;
        ui_->surfaceSize(width, height, scale);

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        bool drew = false;
        ui_->readFrame([&](const uapmd_plugin_hosting::FramebufferView& view) {
            ensureTexture(view.width, view.height);
            if (!texture_ || !texture_->Pixels)
                return;
            if (view.serial != uploaded_serial_) {
                // The plugin draws BGRA; Dear ImGui wants RGBA. The swizzle rides along
                // with the copy that has to happen anyway.
                auto* dst = texture_->Pixels;
                for (uint32_t y = 0; y < view.height; y++) {
                    const uint8_t* src = view.pixels + static_cast<size_t>(y) * view.stride;
                    uint8_t* row = dst + static_cast<size_t>(y) * texture_->GetPitch();
                    for (uint32_t x = 0; x < view.width; x++) {
                        row[x * 4 + 0] = src[x * 4 + 2];
                        row[x * 4 + 1] = src[x * 4 + 1];
                        row[x * 4 + 2] = src[x * 4 + 0];
                        row[x * 4 + 3] = 0xFF;   // JSFX draws opaque; its alpha is unused
                    }
                }
                const ImTextureRect whole{0, 0, (unsigned short) view.width,
                                          (unsigned short) view.height};
                texture_->UpdateRect = whole;
                texture_->Updates.resize(0);
                texture_->Updates.push_back(whole);
                texture_->UsedRect = whole;
                // Only a texture the backend has already created can be *updated*. Asking
                // for an update on one still waiting to be created sends the backend down
                // its update path, where it binds a texture id that does not exist yet and
                // never assigns one -- which trips the assert in ImDrawCmd::GetTexID().
                // A pending create uploads the whole buffer anyway, so leave it alone.
                if (texture_->Status == ImTextureStatus_OK)
                    texture_->SetStatus(ImTextureStatus_WantUpdates);
                uploaded_serial_ = view.serial;
            }
            ImGui::Image(texture_->GetTexRef(),
                         ImVec2(static_cast<float>(view.width), static_cast<float>(view.height)));
            drew = true;
        });

        if (!drew) {
            ImGui::TextUnformatted("Waiting for the plugin to draw...");
            ImGui::Dummy(available);
        } else {
            deliverInputFrom(origin, width, height);
        }

        renderPendingMenu();
        ImGui::End();
        return open_;
    }

    void JsfxEditorPanel::requestMenu(const std::string& spec, int32_t x, int32_t y,
                                      std::function<void(int32_t)> completed) {
        (void) x;
        (void) y;   // Dear ImGui opens the popup where the mouse is, which is where JSFX wants it.
        std::lock_guard lock{menu_mutex_};
        if (menu_open_ || menu_requested_) {
            // Only one menu can be open at a time; a second request is refused rather than
            // queued, because the script waiting on it would otherwise wait for two.
            if (completed)
                completed(0);
            return;
        }
        menu_spec_ = spec;
        menu_completion_ = std::move(completed);
        menu_requested_ = true;
    }

    void JsfxEditorPanel::setCursor(int32_t cursor) {
        (void) cursor;
        // JSFX cursor identifiers are Win32 ones and mostly have no Dear ImGui equivalent.
        // Leaving the cursor alone is better than guessing at a mapping.
    }

    std::string JsfxEditorPanel::droppedFile(int32_t index) {
        (void) index;
        return {};
    }

}

#endif
