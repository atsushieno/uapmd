#pragma once

#ifndef UAPMD_HAS_REMOTE_ENGINE
    #define UAPMD_HAS_REMOTE_ENGINE 1
#endif

#include "EngineMessages.hpp"
#include "ThreadedEngineProxy.hpp"
#include "IpcProtocol.hpp"
#include "SharedAudioRegion.hpp"

#if UAPMD_HAS_REMOTE_ENGINE
    #include "RemoteEngineProxy.hpp"
    #include "EngineServer.hpp"
#endif
