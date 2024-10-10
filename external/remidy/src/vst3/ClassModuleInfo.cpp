
#include <vector>
#include <fstream>
#include <sstream>
#include "ClassModuleInfo.hpp"

#include <priv/common.hpp>

#include "moduleinfoparser.h"

#include "../utils.hpp"

namespace remidy_vst3 {
    std::filesystem::path getModuleInfoFile(std::filesystem::path& bundlePath) {
        // obsolete file path
        std::filesystem::path p2{bundlePath};
        p2.append("Contents").append("moduleinfo.json");
        if (exists(p2))
            return p2;
        // standard file path
        std::filesystem::path p{bundlePath};
        p.append("Contents").append("Resources").append("moduleinfo.json");
        return p;
    }
    bool hasModuleInfo(std::filesystem::path& bundlePath) {
        return exists(getModuleInfoFile(bundlePath));
    }

    std::vector<PluginClassInfo> getModuleInfo(std::filesystem::path& bundlePath) {
        std::vector<PluginClassInfo> list;
        auto info = getModuleInfoFile(bundlePath);
        if (!std::filesystem::exists(info))
            return list;
        std::ifstream ifs{info.string()};
        std::ostringstream ofs;
        ofs << ifs.rdbuf();
        ifs.close();
        std::string str = ofs.str();

        std::ostringstream errorStream{};
        auto moduleInfo = Steinberg::ModuleInfoLib::parseJson(str, &errorStream);
        if (!moduleInfo.has_value()) {
            remidy::Logger::global()->logWarning("Failed to parse moduleinfo.json in %s : %s", bundlePath.c_str(), errorStream.str().c_str());
            return list; // failed to parse
        }
        auto factoryInfo = moduleInfo.value().factoryInfo;
        for (auto& cls : moduleInfo.value().classes) {
            if (strcmp(cls.category.c_str(), kVstAudioEffectClass))
                continue;
            std::string cid = stringToHexBinary(cls.cid);
            v3_tuid tuid{};
            memcpy(tuid, cid.c_str(), cid.length());
            std::string name = cls.name;
            std::string vendor = cls.vendor;
            auto entry = PluginClassInfo{bundlePath, cls.vendor, factoryInfo.url, cls.name, tuid};
            list.emplace_back(std::move(entry));
        };
        return list;
    }
}
