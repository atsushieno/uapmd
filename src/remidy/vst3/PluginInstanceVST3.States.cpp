#include "PluginFormatVST3.hpp"
#include <cstring>

namespace remidy {

namespace {
    struct Vst3StateHeader {
        char magic[4];
        uint32_t version;
        uint32_t componentSize;
        uint32_t controllerSize;
    };
    constexpr char Vst3StateMagic[4] {'R','S','T','3'};
    constexpr uint32_t Vst3StateVersion{1};
}

    std::vector<uint8_t> PluginInstanceVST3::PluginStatesVST3::getState(StateContextType stateContextType,
                                                        bool includeUiState) {
        std::vector<uint8_t> componentState;
        std::vector<uint8_t> controllerState;
        EventLoop::runTaskOnMainThread([&] {
        auto logger = owner->owner->getLogger();
        auto component = owner->component;
        if (!component) {
            logger->logError("PluginStatesVST3::getState(): component is null");
            return;
        }
        VectorStream componentStream{componentState};
        auto result = component->getState((IBStream*) &componentStream);
        if (result != kResultOk) {
            logger->logError("PluginStatesVST3::getState(): failed to get component state: %d", result);
            componentState.clear();
            return;
        }

        auto controller = owner->controller;
        if (!controller)
            return;
        VectorStream controllerStream{controllerState};
        result = controller->getState((IBStream*) &controllerStream);
        if (result == kNotImplemented) {
            logger->logInfo("PluginStatesVST3::getState(): controller does not implement getState()");
            controllerState.clear();
        } else if (result != kResultOk) {
            logger->logError("PluginStatesVST3::getState(): failed to get controller state: %d", result);
            controllerState.clear();
        }
        });

        if (controllerState.empty()) {
            return componentState;
        }

        Vst3StateHeader header{{'R','S','T','3'}, Vst3StateVersion,
                               static_cast<uint32_t>(componentState.size()),
                               static_cast<uint32_t>(controllerState.size())};
        std::vector<uint8_t> combined;
        combined.resize(sizeof(header) + componentState.size() + controllerState.size());
        memcpy(combined.data(), &header, sizeof(header));
        memcpy(combined.data() + sizeof(header), componentState.data(), componentState.size());
        memcpy(combined.data() + sizeof(header) + componentState.size(), controllerState.data(), controllerState.size());
        return combined;
    }

    void PluginInstanceVST3::PluginStatesVST3::setState(std::vector<uint8_t> &state,
                                                        StateContextType stateContextType,
                                                        bool includeUiState) {
        auto logger = owner->owner->getLogger();
        std::vector<uint8_t> componentState;
        std::vector<uint8_t> controllerState;
        bool hasHeader = false;
        if (state.size() >= sizeof(Vst3StateHeader)) {
            auto header = reinterpret_cast<const Vst3StateHeader*>(state.data());
            if (memcmp(header->magic, Vst3StateMagic, sizeof(header->magic)) == 0 &&
                header->version == Vst3StateVersion) {
                auto totalSize = sizeof(Vst3StateHeader) + header->componentSize + header->controllerSize;
                if (state.size() >= totalSize) {
                    componentState.assign(state.begin() + sizeof(Vst3StateHeader),
                                          state.begin() + sizeof(Vst3StateHeader) + header->componentSize);
                    if (header->controllerSize > 0) {
                        controllerState.assign(state.begin() + sizeof(Vst3StateHeader) + header->componentSize,
                                               state.begin() + totalSize);
                    }
                    hasHeader = true;
                } else {
                    logger->logWarning("PluginStatesVST3::setState(): truncated state blob (expected %u bytes, got %zu)",
                                       header->componentSize + header->controllerSize, state.size() - sizeof(Vst3StateHeader));
                }
            }
        }
        if (!hasHeader) {
            componentState = state;
            controllerState.clear();
        }

        bool resumeProcessing = false;
        if (owner->processingActive && owner->processor) {
            auto stopResult = owner->processor->setProcessing(false);
            if (stopResult == kResultOk) {
                owner->processingActive = false;
                resumeProcessing = true;
            } else if (stopResult != kNotImplemented) {
                logger->logWarning("PluginStatesVST3::setState(): failed to stop processing before state load: %d", stopResult);
            }
        }

        EventLoop::runTaskOnMainThread([&] {
        auto component = owner->component;
        if (!component) {
            logger->logError("PluginStatesVST3::setState(): component is null");
            return;
        }

        bool reactivateComponent = owner->componentActive;
        if (reactivateComponent) {
            auto deactivateResult = component->setActive(false);
            if (deactivateResult != kResultOk) {
                logger->logWarning("PluginStatesVST3::setState(): failed to deactivate component before state load: %d", deactivateResult);
                reactivateComponent = false;
            } else {
                owner->componentActive = false;
            }
        }

        VectorStream componentStream{componentState};
        auto result = component->setState((IBStream*) &componentStream);
        if (result != kResultOk) {
            logger->logError("PluginStatesVST3::setState(): failed to set component state: %d", result);
        } else {
            auto controller = owner->controller;
            if (controller) {
                VectorStream controllerComponentStream{componentState};
                result = controller->setComponentState((IBStream*) &controllerComponentStream);
                if (result != kResultOk) {
                    logger->logError("PluginStatesVST3::setState(): failed to set controller component state: %d", result);
                } else if (!controllerState.empty()) {
                    VectorStream controllerStateStream{controllerState};
                    result = controller->setState((IBStream*) &controllerStateStream);
                    if (result != kResultOk)
                        logger->logError("PluginStatesVST3::setState(): failed to set controller UI state: %d", result);
                }
            }
        }

        if (reactivateComponent) {
            auto activateResult = component->setActive(true);
            if (activateResult != kResultOk)
                logger->logWarning("PluginStatesVST3::setState(): failed to reactivate component after state load: %d", activateResult);
            else
                owner->componentActive = true;
        }
        });

        if (resumeProcessing && owner->processor) {
            auto startResult = owner->processor->setProcessing(true);
            if (startResult != kResultOk && startResult != kNotImplemented)
                logger->logWarning("PluginStatesVST3::setState(): failed to restart processing after state load: %d", startResult);
            else if (startResult == kResultOk)
                owner->processingActive = true;
        }
    }
}
