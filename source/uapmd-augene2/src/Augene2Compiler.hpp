#pragma once

#include <map>
#include <string>
#include <vector>

#include <uapmd-data/uapmd-data.hpp>

namespace uapmd_augene2 {

struct Source {
    std::string path;
    std::string text;
    std::string external_path;
    bool compile{false};
    bool operator==(const Source&) const = default;
};

struct CompiledClip {
    std::string key;
    std::string name;
    int64_t position_ticks{};
    bool master{false};
    uapmd::MidiClipReader::ClipInfo content;
};

struct Compilation {
    std::vector<Source> sources;
    std::vector<CompiledClip> clips;
    std::vector<std::string> diagnostics;
    bool success{false};
    bool attempted{false};
};

// Compilation and optional source refresh run exclusively on a worker.
Compilation compileSources(std::vector<Source> sources, bool refresh, bool onlyIfChanged = false);

}
