#include "remidy-gui/priv/ContainerWindow.hpp"

namespace remidy::gui {

    class AndroidContainerWindow : public ContainerWindow {
    public:
        ~AndroidContainerWindow() override = default;

        void show(bool visible) override {}
        void resize(int width, int height) override {}
        void setResizeCallback(std::function<void(int, int)> callback) override {}
        void setResizable(bool resizable) override {}
        Bounds getBounds() const override { return {}; }
        void* getHandle() const override { return nullptr; }
    };

    std::unique_ptr<ContainerWindow> ContainerWindow::create(const char* title, int width, int height, std::function<void()> closeCallback) {
        throw std::runtime_error("AndroidContainerWindow::create not implemented");
    }
}
