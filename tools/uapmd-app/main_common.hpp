#pragma once

namespace uapmd {
    /**
     * Main application loop - called by both desktop and Android entrypoints.
     *
     * @param argc Number of command-line arguments
     * @param argv Array of command-line argument strings
     * @return EXIT_SUCCESS on successful completion, EXIT_FAILURE on error
     */
    int runMainLoop(int argc, char** argv);

} // namespace uapmd
