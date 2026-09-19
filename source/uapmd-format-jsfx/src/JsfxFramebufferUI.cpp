#include "JsfxFramebufferUI.hpp"

#include <algorithm>
#include <cstring>

namespace uapmd_jsfx {

    namespace {
        constexpr uint32_t kMinimumSize = 1;
        constexpr uint32_t kMaximumSize = 8192;

        uint32_t clampSize(uint32_t value) {
            return std::clamp(value, kMinimumSize, kMaximumSize);
        }

        // Everything below turns what a host reports into what ysfx expects. It lives on
        // this side of the extension so that a host never has to learn any of it, and so
        // that there is one copy of it however many hosts there are.

        uint32_t translateModifiers(uint32_t modifiers) {
            uint32_t out = 0;
            if (modifiers & uapmd_plugin_hosting::kFramebufferModifierShift)
                out |= ysfx_mod_shift;
            if (modifiers & uapmd_plugin_hosting::kFramebufferModifierControl)
                out |= ysfx_mod_ctrl;
            if (modifiers & uapmd_plugin_hosting::kFramebufferModifierAlt)
                out |= ysfx_mod_alt;
            if (modifiers & uapmd_plugin_hosting::kFramebufferModifierSuper)
                out |= ysfx_mod_super;
            return out;
        }

        // ysfx's button bits are not in the order a script sees them: it maps left/middle/
        // right (1/2/4) onto mouse_cap 1/64/2 itself. Sending a script's own numbering
        // here delivers a right click as a middle one.
        uint32_t translateButtons(uint32_t buttons) {
            uint32_t out = 0;
            if (buttons & uapmd_plugin_hosting::kFramebufferButtonLeft)
                out |= ysfx_button_left;
            if (buttons & uapmd_plugin_hosting::kFramebufferButtonRight)
                out |= ysfx_button_right;
            if (buttons & uapmd_plugin_hosting::kFramebufferButtonMiddle)
                out |= ysfx_button_middle;
            return out;
        }

        uint32_t translateKey(const uapmd_plugin_hosting::FramebufferKeyEvent& event) {
            using Key = uapmd_plugin_hosting::FramebufferKey;
            switch (event.key) {
                case Key::None:      break;
                case Key::Backspace: return ysfx_key_backspace;
                case Key::Tab:       return '\t';
                case Key::Enter:     return '\r';
                case Key::Escape:    return ysfx_key_escape;
                case Key::Space:     return ' ';
                case Key::Delete:    return ysfx_key_delete;
                case Key::Left:      return ysfx_key_left;
                case Key::Right:     return ysfx_key_right;
                case Key::Up:        return ysfx_key_up;
                case Key::Down:      return ysfx_key_down;
                case Key::PageUp:    return ysfx_key_page_up;
                case Key::PageDown:  return ysfx_key_page_down;
                case Key::Home:      return ysfx_key_home;
                case Key::End:       return ysfx_key_end;
                case Key::Insert:    return ysfx_key_insert;
                case Key::F1: case Key::F2: case Key::F3: case Key::F4:
                case Key::F5: case Key::F6: case Key::F7: case Key::F8:
                case Key::F9: case Key::F10: case Key::F11: case Key::F12:
                    return ysfx_key_f1 + (static_cast<uint32_t>(event.key) -
                                          static_cast<uint32_t>(Key::F1));
            }
            // Not a named key, so it is whatever was typed. JSFX deals in single bytes,
            // and folds letters to lower case.
            if (event.character >= 'A' && event.character <= 'Z')
                return event.character - 'A' + 'a';
            return event.character <= 0xFF ? event.character : 0;
        }

        // Cursor identifiers reach a JSFX script from gfx_setcursor() unchanged, and by
        // REAPER's convention they are Win32 IDC_* values.
        uapmd_plugin_hosting::FramebufferCursor translateCursor(int32_t cursor) {
            using Cursor = uapmd_plugin_hosting::FramebufferCursor;
            switch (cursor) {
                case 32513: return Cursor::IBeam;
                case 32514: return Cursor::Wait;
                case 32515: return Cursor::Crosshair;
                case 32642: return Cursor::SizeNWSE;
                case 32643: return Cursor::SizeNESW;
                case 32644: return Cursor::SizeHorizontal;
                case 32645: return Cursor::SizeVertical;
                case 32646: return Cursor::SizeAll;
                case 32648: return Cursor::NotAllowed;
                case 32649: return Cursor::Hand;
                case 32512: default: return Cursor::Arrow;
            }
        }

        // ysfx parses the gfx_showmenu string itself, ids and all, so this only has to
        // walk the instructions it produces and nest the submenus.
        std::vector<uapmd_plugin_hosting::FramebufferMenuItem> parseMenu(const char* spec) {
            std::vector<uapmd_plugin_hosting::FramebufferMenuItem> items{};
            if (!spec || !*spec)
                return items;

            ysfx_menu_t* menu = ysfx_parse_menu(spec);
            if (!menu)
                return items;

            // Submenus nest, so the instruction stream is walked with a stack of the lists
            // being appended to.
            std::vector<std::vector<uapmd_plugin_hosting::FramebufferMenuItem>*> stack{&items};
            for (uint32_t i = 0; i < menu->insn_count; i++) {
                const ysfx_menu_insn_t& insn = menu->insns[i];
                auto& current = *stack.back();
                switch (insn.opcode) {
                    case ysfx_menu_item: {
                        uapmd_plugin_hosting::FramebufferMenuItem item{};
                        item.label = insn.name ? insn.name : "";
                        item.id = static_cast<int32_t>(insn.id);
                        item.disabled = (insn.item_flags & ysfx_menu_item_disabled) != 0;
                        item.checked = (insn.item_flags & ysfx_menu_item_checked) != 0;
                        current.emplace_back(std::move(item));
                        break;
                    }
                    case ysfx_menu_separator: {
                        uapmd_plugin_hosting::FramebufferMenuItem item{};
                        item.separator = true;
                        current.emplace_back(std::move(item));
                        break;
                    }
                    case ysfx_menu_sub: {
                        uapmd_plugin_hosting::FramebufferMenuItem item{};
                        item.label = insn.name ? insn.name : "";
                        item.disabled = (insn.item_flags & ysfx_menu_item_disabled) != 0;
                        current.emplace_back(std::move(item));
                        stack.push_back(&current.back().children);
                        break;
                    }
                    case ysfx_menu_endsub:
                        // Never pop the outermost list: a malformed menu with more ends
                        // than starts would otherwise walk off it.
                        if (stack.size() > 1)
                            stack.pop_back();
                        break;
                }
            }

            ysfx_menu_free(menu);
            return items;
        }
    }

    JsfxFramebufferUI::JsfxFramebufferUI(ysfx_t* fx) : fx_(fx) {
    }

    JsfxFramebufferUI::~JsfxFramebufferUI() {
        stop();
    }

    void JsfxFramebufferUI::start() {
        {
            std::lock_guard lock{state_mutex_};
            if (running_)
                return;
            running_ = true;
        }
        JsfxGfxScheduler::shared().add(this);
    }

    void JsfxFramebufferUI::stop() {
        {
            std::lock_guard lock{state_mutex_};
            if (!running_)
                return;
            running_ = false;
        }
        // Release the worker before asking the scheduler to wait for it. If a script has a
        // menu open, the worker is parked inside tickGfx() and remove() would otherwise
        // wait for the user to make a choice.
        abandonPendingMenu();
        JsfxGfxScheduler::shared().remove(this);
    }

    bool JsfxFramebufferUI::preferredSize(uint32_t& width, uint32_t& height) {
        uint32_t dim[2] = {0, 0};
        if (!ysfx_get_gfx_dim(fx_, dim) || dim[0] == 0 || dim[1] == 0)
            return false;
        width = dim[0];
        height = dim[1];
        return true;
    }

    void JsfxFramebufferUI::surfaceSize(uint32_t width, uint32_t height, double scaleFactor) {
        std::lock_guard lock{state_mutex_};
        const uint32_t w = clampSize(width);
        const uint32_t h = clampSize(height);
        if (w == requested_width_ && h == requested_height_ && scaleFactor == scale_factor_)
            return;
        requested_width_ = w;
        requested_height_ = h;
        scale_factor_ = scaleFactor > 0.0 ? scaleFactor : 1.0;
        size_dirty_ = true;
    }

    bool JsfxFramebufferUI::readFrame(
            const std::function<void(const uapmd_plugin_hosting::FramebufferView&)>& consume) {
        std::lock_guard lock{frame_mutex_};
        if (front_.empty() || width_ == 0 || height_ == 0 || !consume)
            return false;
        uapmd_plugin_hosting::FramebufferView view{};
        view.pixels = front_.data();
        view.width = width_;
        view.height = height_;
        view.stride = width_ * 4;
        view.serial = serial_;
        consume(view);
        return true;
    }

    void JsfxFramebufferUI::deliverInput(uapmd_plugin_hosting::FramebufferInput input) {
        std::lock_guard lock{state_mutex_};
        // Key presses queue up because dropping one loses a keystroke; pointer and window
        // state are a snapshot, because only the latest matters.
        pending_keys_.insert(pending_keys_.end(), input.keys.begin(), input.keys.end());
        input.keys.clear();
        pending_input_ = std::move(input);
    }

    void JsfxFramebufferUI::uiHost(uapmd_plugin_hosting::FramebufferUIHost* host) {
        {
            std::lock_guard lock{state_mutex_};
            host_ = host;
        }
        if (!host)
            abandonPendingMenu();
    }

    void JsfxFramebufferUI::displayed(bool value) {
        {
            std::lock_guard lock{state_mutex_};
            displayed_ = value;
        }
        // Being displayed is what makes the renderer run. A host that draws the frames
        // itself never goes near showUI() -- there is no view for it to create -- so if
        // this did not start the renderer, nothing would, and the editor would stay
        // blank for ever. start() is idempotent, so the showUI() path is unaffected.
        //
        // Not stopped on the way out: a hidden editor keeps ticking slowly so that it
        // notices being shown again, which is what desiredFrameRate() is about.
        if (value)
            start();
    }

    uint32_t JsfxFramebufferUI::desiredFrameRate() {
        {
            std::lock_guard lock{state_mutex_};
            // A hidden editor still ticks, slowly, so that it notices being shown again.
            if (!displayed_)
                return 2;
        }
        return ysfx_get_requested_framerate(fx_);
    }

    int32_t JsfxFramebufferUI::onShowMenu(void* userData, const char* spec, int32_t x, int32_t y) {
        auto* self = static_cast<JsfxFramebufferUI*>(userData);
        if (!self || !spec)
            return 0;

        uapmd_plugin_hosting::FramebufferUIHost* host;
        {
            std::lock_guard lock{self->state_mutex_};
            host = self->host_;
        }
        if (!host)
            return 0;

        uint64_t generation;
        {
            std::lock_guard lock{self->menu_mutex_};
            generation = ++self->menu_generation_;
            self->menu_pending_ = true;
            self->menu_answered_ = false;
            self->menu_answer_ = 0;
        }

        // The host opens the menu and returns; it must not block. The completion may come
        // from any thread, including from inside requestMenu itself.
        //
        // Parsed here rather than by the host: the spec is JSFX's, and so are the rules
        // for what `#`, `!`, `>` and `<` mean and how the ids a script gets back are
        // numbered. ysfx already knows all of that.
        host->requestMenu(parseMenu(spec), x, y, [self, generation](int32_t choice) {
            {
                std::lock_guard lock{self->menu_mutex_};
                // A completion for a menu that was already abandoned is ignored, so a late
                // answer cannot be mistaken for the answer to a newer menu.
                if (self->menu_generation_ != generation || self->menu_answered_)
                    return;
                self->menu_answered_ = true;
                self->menu_answer_ = choice;
                self->menu_pending_ = false;
            }
            self->menu_condition_.notify_all();
        });

        std::unique_lock lock{self->menu_mutex_};
        self->menu_condition_.wait(lock, [self, generation] {
            return self->menu_answered_ || self->menu_generation_ != generation;
        });
        return self->menu_generation_ == generation ? self->menu_answer_ : 0;
    }

    void JsfxFramebufferUI::abandonPendingMenu() {
        {
            std::lock_guard lock{menu_mutex_};
            if (!menu_pending_)
                return;
            // Bumping the generation is what tells the waiter its menu is gone, and makes
            // any answer that arrives afterwards harmless.
            menu_generation_++;
            menu_pending_ = false;
            menu_answered_ = false;
            menu_answer_ = 0;
        }
        menu_condition_.notify_all();
    }

    void JsfxFramebufferUI::onSetCursor(void* userData, int32_t cursor) {
        auto* self = static_cast<JsfxFramebufferUI*>(userData);
        if (!self)
            return;
        std::lock_guard lock{self->state_mutex_};
        if (self->host_)
            self->host_->setCursor(translateCursor(cursor));
    }

    const char* JsfxFramebufferUI::onGetDropFile(void* userData, int32_t index) {
        auto* self = static_cast<JsfxFramebufferUI*>(userData);
        if (!self)
            return nullptr;
        std::lock_guard lock{self->state_mutex_};
        if (!self->host_)
            return nullptr;
        self->drop_file_returned_ = self->host_->droppedFile(index);
        if (index < 0 || self->drop_file_returned_.empty())
            return nullptr;
        // ysfx reads this before the next call, so one buffer owned here is enough.
        return self->drop_file_returned_.c_str();
    }

    void JsfxFramebufferUI::tickGfx() {
        uint32_t width;
        uint32_t height;
        double scale;
        bool resized;
        bool displayed;
        uapmd_plugin_hosting::FramebufferInput input{};
        std::vector<uapmd_plugin_hosting::FramebufferKeyEvent> keys{};
        {
            std::lock_guard lock{state_mutex_};
            width = requested_width_;
            height = requested_height_;
            scale = scale_factor_;
            resized = size_dirty_;
            size_dirty_ = false;
            displayed = displayed_;
            input = pending_input_;
            keys.swap(pending_keys_);
        }

        if (width == 0 || height == 0)
            return;

        if (resized || back_.size() != static_cast<size_t>(width) * height * 4) {
            back_.assign(static_cast<size_t>(width) * height * 4, 0);
        }

        // ysfx draws into the buffer we hand it rather than one of its own, so this points
        // it at the back buffer and has to be redone whenever that buffer moves.
        ysfx_gfx_config_t config{};
        config.user_data = this;
        config.pixel_width = width;
        config.pixel_height = height;
        config.pixel_stride = width * 4;
        config.pixels = back_.data();
        config.scale_factor = static_cast<ysfx_real>(scale);
        config.show_menu = &JsfxFramebufferUI::onShowMenu;
        config.set_cursor = &JsfxFramebufferUI::onSetCursor;
        config.get_drop_file = &JsfxFramebufferUI::onGetDropFile;
        ysfx_gfx_setup(fx_, &config);

        ysfx_gfx_set_window_state(fx_, input.hasFocus, displayed, input.pointerOver);
        for (auto& key : keys)
            ysfx_gfx_add_key(fx_, translateModifiers(key.modifiers), translateKey(key),
                             key.pressed);
        ysfx_gfx_update_mouse(fx_, translateModifiers(input.modifiers),
                              input.pointerX, input.pointerY,
                              translateButtons(input.buttons),
                              static_cast<ysfx_real>(input.wheel),
                              static_cast<ysfx_real>(input.horizontalWheel));

        const bool changed = ysfx_gfx_run(fx_);
        if (!changed && !resized)
            return;

        std::lock_guard lock{frame_mutex_};
        front_.swap(back_);
        width_ = width;
        height_ = height;
        serial_++;
        // The new back buffer is the old front one, which may be the wrong size after a
        // resize; the next tick reallocates it.
    }

}
