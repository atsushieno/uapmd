#include "PluginFormatVST3.hpp"

namespace remidy {

    std::vector<uint8_t> PluginInstanceVST3::PluginStatesVST3::getState(StateContextType stateContextType,
                                                        bool includeUiState) {
        std::vector<uint8_t> componentState;
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
        });

        return componentState;
    }

    void PluginInstanceVST3::PluginStatesVST3::setState(std::vector<uint8_t> &state,
                                                        StateContextType stateContextType,
                                                        bool includeUiState) {
        auto logger = owner->owner->getLogger();

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

        VectorStream componentStream{state};
        auto result = component->setState((IBStream*) &componentStream);
        if (result != kResultOk) {
            logger->logError("PluginStatesVST3::setState(): failed to set component state: %d", result);
        } else {
            auto controller = owner->controller;
            if (controller) {
                VectorStream controllerComponentStream{state};
                result = controller->setComponentState((IBStream*) &controllerComponentStream);
                if (result != kResultOk) {
                    logger->logError("PluginStatesVST3::setState(): failed to set controller component state: %d", result);
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
