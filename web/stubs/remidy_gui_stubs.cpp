#include <remidy-gui/remidy-gui.hpp>
#include <memory>

namespace remidy::gui {

class StubContainerWindow : public ContainerWindow {
public:
    void show(bool) override {}
    void resize(int, int) override {}
    void setResizeCallback(std::function<void(int,int)>) override {}
    void setResizable(bool) override {}
    Bounds getBounds() const override { return Bounds{}; }
    void* getHandle() const override { return nullptr; }
};

std::unique_ptr<ContainerWindow> ContainerWindow::create(const char*, int, int, std::function<void()>) {
    return std::make_unique<StubContainerWindow>();
}

}
