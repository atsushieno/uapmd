// iOS SDL3 entry point for UAPMD
// SDL3 provides the UIApplicationDelegate infrastructure on iOS.
// Shared application logic is in main_common.cpp.

#include "main_common.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

// SDL3 automatically redirects main() to SDL_main on iOS and provides
// UIApplicationDelegate. We define main() here and SDL3 handles the rest.
int main(int argc, char** argv) {
    return uapmd::runMainLoop(argc, argv);
}
