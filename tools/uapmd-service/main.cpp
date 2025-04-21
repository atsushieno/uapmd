
#include "Shell/CommandShell.hpp"

int main(int argc, const char** argv) {
    remidy::EventLoop::initializeOnUIThread();
    auto shell = uapmd::CommandShell::create(argc, argv);
    return shell->run();
}
