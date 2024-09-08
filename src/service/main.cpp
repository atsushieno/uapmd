#include <iostream>

#include "Shell/CommandShell.h"

int main(int argc, const char** argv) {
    auto shell = CommandShell::create(argc, argv);
    return shell->run();
}
