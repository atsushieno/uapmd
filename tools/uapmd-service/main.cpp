
#include "Shell/CommandShell.hpp"

int main(int argc, const char** argv) {
    auto shell = uapmd::CommandShell::create(argc, argv);
    return shell->run();
}
