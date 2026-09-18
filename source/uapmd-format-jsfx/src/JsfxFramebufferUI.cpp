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
        std::lock_guard lock{state_mutex_};
        displayed_ = value;
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
        host->requestMenu(std::string{spec}, x, y, [self, generation](int32_t choice) {
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
            self->host_->setCursor(cursor);
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
            ysfx_gfx_add_key(fx_, key.modifiers, key.key, key.pressed);
        ysfx_gfx_update_mouse(fx_, input.modifiers, input.pointerX, input.pointerY,
                              input.buttons, static_cast<ysfx_real>(input.wheel),
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
