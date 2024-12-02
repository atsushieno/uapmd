
#include "AppModel.hpp"

#define DEFAULT_UMP_BUFFER_SIZE 65536
#define DEFAULT_SAMPLE_RATE 48000
uapmd::AppModel model{DEFAULT_UMP_BUFFER_SIZE, DEFAULT_SAMPLE_RATE};

uapmd::AppModel& uapmd::AppModel::instance() { return model; }
