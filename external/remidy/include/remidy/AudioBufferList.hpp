#pragma once
#include <cstdint>

namespace remidy {

    class AudioBufferList {
    public:
        float* getFloatBufferForChannel(int32_t channel);
        double* getDoubleBufferForChannel(int32_t channel);
        int32_t size();
    };

}
