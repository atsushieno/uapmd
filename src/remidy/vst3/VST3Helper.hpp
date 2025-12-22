#pragma once

#include <codecvt>
#include <filesystem>
#include <functional>
#include <vector>
#include <string>

// VST3SDK C++ interface includes
#include <pluginterfaces/base/ftypes.h>
#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstunits.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstpluginterfacesupport.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstmidimapping2.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/ivstattributes.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/gui/iplugviewcontentscalesupport.h>
#include <public.sdk/source/vst/hosting/connectionproxy.h>

#include <priv/common.hpp>// for Logger
#include "ClassModuleInfo.hpp"

// Use Steinberg namespaces directly
using namespace Steinberg;
using namespace Steinberg::Vst;

namespace remidy_vst3 {

    std::string vst3StringToStdString(String128& src);

    IPluginFactory* getFactoryFromLibrary(void* module);

    void forEachPlugin(std::filesystem::path vst3Dir,
        std::function<void(void* module, IPluginFactory* factory, PluginClassInfo& info)> func,
        std::function<void(void* module)> cleanup
    );
}
