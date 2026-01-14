#pragma once

#include <memory>
#include <string>
#include "AudioFileReader.hpp"

namespace uapmd {

    // Create an AudioFileReader for the given file path.
    // Returns nullptr if the format is unsupported or cannot be opened.
    std::unique_ptr<AudioFileReader> createAudioFileReaderFromPath(const std::string& filepath);

}

