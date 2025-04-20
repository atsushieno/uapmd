
#include "AppModel.hpp"

#define DEFAULT_AUDIO_BUFFER_SIZE 1024
#define DEFAULT_UMP_BUFFER_SIZE 65536
#define DEFAULT_SAMPLE_RATE 48000

uapmd::AppModel& uapmd::AppModel::instance() {
    static AppModel model{DEFAULT_AUDIO_BUFFER_SIZE, DEFAULT_UMP_BUFFER_SIZE, DEFAULT_SAMPLE_RATE};
    return model;
}

uapmd::AppModel::AppModel(size_t audioBufferSizeInFrames, size_t umpBufferSizeInBytes, int32_t sampleRate) :
        sequencer_(audioBufferSizeInFrames, umpBufferSizeInBytes, sampleRate) {
}
