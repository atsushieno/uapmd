// Android SDL3 entry point for UAPMD
// SDL3 provides the Activity infrastructure and main entry point
// Shared application logic is in main_common.cpp

#include "main_common.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

// SDL3 automatically provides the entry point for Android
// We just need to define main() which SDL3 will call
int main(int argc, char** argv) {
    // Run the shared main application loop
    // SDL3 handles all Android lifecycle, windowing, and input
    return uapmd::runMainLoop(argc, argv);
}
