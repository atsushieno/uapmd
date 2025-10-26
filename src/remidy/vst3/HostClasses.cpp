#include <thread>

#include "HostClasses.hpp"
#if !SMTG_OS_WINDOWS
#include <sys/select.h>
#endif
#include <priv/event-loop.hpp>
#include <public.sdk/source/vst/utility/stringconvert.h>

namespace remidy_vst3 {

    std::string vst3StringToStdString(String128& src) {
        return Steinberg::Vst::StringConvert::convert(src);
    }

    const std::basic_string<char16_t> HostApplication::name16t{Steinberg::Vst::StringConvert::convert("remidy")};

    HostApplication::HostApplication(remidy::Logger* logger): logger(logger) {
        // Instantiate nested implementation classes
        event_handler = new EventHandlerImpl(this);
        handler = new ComponentHandlerImpl(this);
        handler2 = new ComponentHandler2Impl(this);
        unit_handler = new UnitHandlerImpl(this);
        message = new MessageImpl(this);
        plug_frame = new PlugFrameImpl(this);
        support = new PlugInterfaceSupportImpl(this);
        run_loop = new RunLoopImpl(this);
    }

    HostApplication::~HostApplication() {
        // Clean up nested implementation classes
        if (event_handler) event_handler->release();
        if (handler) handler->release();
        if (handler2) handler2->release();
        if (unit_handler) unit_handler->release();
        if (message) message->release();
        if (plug_frame) plug_frame->release();
        if (support) support->release();
        if (run_loop) run_loop->release();
    }

    void HostApplication::startProcessing() {
        parameter_changes.startProcessing();
    }

    void HostApplication::stopProcessing() {
        parameter_changes.stopProcessing();
    }

    tresult PLUGIN_API HostApplication::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IHostApplication)
        QUERY_INTERFACE(_iid, obj, IHostApplication::iid, IHostApplication)

        // Return nested interface implementations
        if (FUnknownPrivate::iidEqual(_iid, IComponentHandler::iid)) {
            if (handler) handler->addRef();
            *obj = handler;
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IComponentHandler2::iid)) {
            if (handler2) handler2->addRef();
            *obj = handler2;
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IMessage::iid)) {
            if (message) message->addRef();
            *obj = message;
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IPlugFrame::iid)) {
            if (plug_frame) plug_frame->addRef();
            *obj = plug_frame;
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IUnitHandler::iid)) {
            if (unit_handler) unit_handler->addRef();
            *obj = unit_handler;
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IPlugInterfaceSupport::iid)) {
            if (support) support->addRef();
            *obj = support;
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IRunLoop::iid)) {
            if (run_loop) run_loop->addRef();
            *obj = run_loop;
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IParameterChanges::iid)) {
            auto iface = parameter_changes.asInterface();
            iface->addRef();
            *obj = iface;
            return kResultOk;
        }

        *obj = nullptr;
        return kNoInterface;
    }

    tresult PLUGIN_API HostApplication::getName(String128 name) {
        name16t.copy((char16_t*) name, name16t.length());
        name[name16t.length()] = 0;
        return kResultOk;
    }

    tresult PLUGIN_API HostApplication::createInstance(TUID cid, TUID _iid, void** obj) {
        *obj = nullptr;
        if (FUnknownPrivate::iidEqual(cid, IAttributeList::iid))
            *obj = new HostAttributeList();
        else if (FUnknownPrivate::iidEqual(cid, IMessage::iid))
            *obj = new HostMessage();
        else
            return kNoInterface;
        return kResultOk;
    }

    // EventHandlerImpl
    tresult PLUGIN_API HostApplication::EventHandlerImpl::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IEventHandler)
        QUERY_INTERFACE(_iid, obj, IEventHandler::iid, IEventHandler)
        // Forward to owner
        if (owner)
            return owner->queryInterface(_iid, obj);
        *obj = nullptr;
        return kNoInterface;
    }

    void PLUGIN_API HostApplication::EventHandlerImpl::onFDIsSet(int fd) {
        // Event handler implementation - currently not used
    }

    // ComponentHandlerImpl
    tresult PLUGIN_API HostApplication::ComponentHandlerImpl::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IComponentHandler)
        QUERY_INTERFACE(_iid, obj, IComponentHandler::iid, IComponentHandler)
        if (owner)
            return owner->queryInterface(_iid, obj);
        *obj = nullptr;
        return kNoInterface;
    }

    tresult PLUGIN_API HostApplication::ComponentHandlerImpl::beginEdit(ParamID id) {
        // Begin parameter edit - currently not implemented
        return kResultOk;
    }

    tresult PLUGIN_API HostApplication::ComponentHandlerImpl::performEdit(ParamID id, ParamValue valueNormalized) {
        if (!owner)
            return kInvalidArgument;
        
        // Find and invoke parameter edit handler if registered
        for (auto& pair : owner->parameter_edit_handlers) {
            if (pair.second)
                pair.second(id, valueNormalized);
        }
        return kResultOk;
    }

    tresult PLUGIN_API HostApplication::ComponentHandlerImpl::endEdit(ParamID id) {
        // End parameter edit - currently not implemented
        return kResultOk;
    }

    tresult PLUGIN_API HostApplication::ComponentHandlerImpl::restartComponent(int32 flags) {
        // Restart component - currently not implemented
        return kResultOk;
    }

    // ComponentHandler2Impl
    tresult PLUGIN_API HostApplication::ComponentHandler2Impl::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IComponentHandler2)
        QUERY_INTERFACE(_iid, obj, IComponentHandler::iid, IComponentHandler2)
        QUERY_INTERFACE(_iid, obj, IComponentHandler2::iid, IComponentHandler2)
        if (owner)
            return owner->queryInterface(_iid, obj);
        *obj = nullptr;
        return kNoInterface;
    }

    tresult PLUGIN_API HostApplication::ComponentHandler2Impl::setDirty(TBool state) {
        // Set dirty state - currently not implemented
        return kResultOk;
    }

    tresult PLUGIN_API HostApplication::ComponentHandler2Impl::requestOpenEditor(FIDString name) {
        // Request open editor - currently not implemented
        return kResultOk;
    }

    tresult PLUGIN_API HostApplication::ComponentHandler2Impl::startGroupEdit() {
        // Start group edit - currently not implemented
        return kResultOk;
    }

    tresult PLUGIN_API HostApplication::ComponentHandler2Impl::finishGroupEdit() {
        // Finish group edit - currently not implemented
        return kResultOk;
    }

    // UnitHandlerImpl
    tresult PLUGIN_API HostApplication::UnitHandlerImpl::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IUnitHandler)
        QUERY_INTERFACE(_iid, obj, IUnitHandler::iid, IUnitHandler)
        if (owner)
            return owner->queryInterface(_iid, obj);
        *obj = nullptr;
        return kNoInterface;
    }

    tresult PLUGIN_API HostApplication::UnitHandlerImpl::notifyUnitSelection(UnitID unitId) {
        // Notify unit selection - currently not implemented
        return kResultOk;
    }

    tresult PLUGIN_API HostApplication::UnitHandlerImpl::notifyProgramListChange(ProgramListID listId, int32 programIndex) {
        // Notify program list change - currently not implemented
        return kResultOk;
    }

    // MessageImpl
    tresult PLUGIN_API HostApplication::MessageImpl::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IMessage)
        QUERY_INTERFACE(_iid, obj, IMessage::iid, IMessage)
        if (owner)
            return owner->queryInterface(_iid, obj);
        *obj = nullptr;
        return kNoInterface;
    }

    FIDString PLUGIN_API HostApplication::MessageImpl::getMessageID() {
        return message_id.empty() ? nullptr : message_id.c_str();
    }

    void PLUGIN_API HostApplication::MessageImpl::setMessageID(FIDString id) {
        message_id = id ? id : "";
    }

    IAttributeList* PLUGIN_API HostApplication::MessageImpl::getAttributes() {
        return attributes;
    }

    // PlugFrameImpl
    tresult PLUGIN_API HostApplication::PlugFrameImpl::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IPlugFrame)
        QUERY_INTERFACE(_iid, obj, IPlugFrame::iid, IPlugFrame)
        if (owner)
            return owner->queryInterface(_iid, obj);
        *obj = nullptr;
        return kNoInterface;
    }

    tresult PLUGIN_API HostApplication::PlugFrameImpl::resizeView(IPlugView* view, ViewRect* newSize) {
        if (!owner || !view || !newSize)
            return kInvalidArgument;

        // Check if there's a resize handler registered for this view
        auto it = owner->resize_request_handlers.find(view);
        if (it != owner->resize_request_handlers.end() && it->second) {
            uint32_t width = newSize->right - newSize->left;
            uint32_t height = newSize->bottom - newSize->top;
            if (it->second(width, height))
                return kResultOk;
            return kResultFalse;
        }

        return kResultFalse;
    }

    // PlugInterfaceSupportImpl
    tresult PLUGIN_API HostApplication::PlugInterfaceSupportImpl::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IPlugInterfaceSupport)
        QUERY_INTERFACE(_iid, obj, IPlugInterfaceSupport::iid, IPlugInterfaceSupport)
        if (owner)
            return owner->queryInterface(_iid, obj);
        *obj = nullptr;
        return kNoInterface;
    }

    tresult PLUGIN_API HostApplication::PlugInterfaceSupportImpl::isPlugInterfaceSupported(const TUID _iid) {
        // Check if we support the requested interface
        if (FUnknownPrivate::iidEqual(_iid, IComponentHandler::iid)) return kResultOk;
        if (FUnknownPrivate::iidEqual(_iid, IComponentHandler2::iid)) return kResultOk;
        if (FUnknownPrivate::iidEqual(_iid, IUnitHandler::iid)) return kResultOk;
        if (FUnknownPrivate::iidEqual(_iid, IPlugFrame::iid)) return kResultOk;
        if (FUnknownPrivate::iidEqual(_iid, IMessage::iid)) return kResultOk;
        if (FUnknownPrivate::iidEqual(_iid, IAttributeList::iid)) return kResultOk;
        if (FUnknownPrivate::iidEqual(_iid, IRunLoop::iid)) return kResultOk;
        return kResultFalse;
    }

    // RunLoopImpl
    tresult PLUGIN_API HostApplication::RunLoopImpl::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IRunLoop)
        QUERY_INTERFACE(_iid, obj, IRunLoop::iid, IRunLoop)
        if (owner)
            return owner->queryInterface(_iid, obj);
        *obj = nullptr;
        return kNoInterface;
    }

    tresult PLUGIN_API HostApplication::RunLoopImpl::registerEventHandler(IEventHandler* handler, FileDescriptor fd) {
        if (!owner || !handler)
            return kInvalidArgument;

        std::lock_guard<std::mutex> lock(owner->event_handlers_mutex);

        auto info = std::make_shared<HostApplication::EventHandlerInfo>();
        info->handler = handler;
        info->fd = fd;
        info->active.store(true);

        owner->event_handlers.push_back(info);

        // Start a background thread to monitor this file descriptor
        std::thread monitor_thread([info]() {
            fd_set readfds;
            struct timeval tv;

            while (info->active.load()) {
                FD_ZERO(&readfds);
                FD_SET(info->fd, &readfds);

                // Timeout for select so we can check active flag periodically
                tv.tv_sec = 0;
                tv.tv_usec = 100000;  // 100ms

                int result = select(info->fd + 1, &readfds, nullptr, nullptr, &tv);

                if (result > 0 && FD_ISSET(info->fd, &readfds)) {
                    // File descriptor has data available
                    remidy::EventLoop::runTaskOnMainThread([info]() {
                        if (!info->active.load()) return;

                        auto handler = info->handler;
                        if (handler) {
                            handler->onFDIsSet(info->fd);
                        }
                    });
                }
            }
        });
        monitor_thread.detach();

        return kResultOk;
    }

    tresult PLUGIN_API HostApplication::RunLoopImpl::unregisterEventHandler(IEventHandler* handler) {
        if (!owner || !handler)
            return kInvalidArgument;

        std::lock_guard<std::mutex> lock(owner->event_handlers_mutex);
        
        auto it = owner->event_handlers.begin();
        while (it != owner->event_handlers.end()) {
            if ((*it)->handler == handler) {
                (*it)->active.store(false);
                it = owner->event_handlers.erase(it);
                return kResultOk;
            } else {
                ++it;
            }
        }

        return kInvalidArgument;
    }

    tresult PLUGIN_API HostApplication::RunLoopImpl::registerTimer(ITimerHandler* handler, TimerInterval milliseconds) {
        if (!owner || !handler)
            return kInvalidArgument;

        std::lock_guard<std::mutex> lock(owner->timers_mutex);
        
        auto timer_info = std::make_shared<HostApplication::TimerInfo>();
        timer_info->handler = handler;
        timer_info->interval_ms = milliseconds;
        timer_info->active.store(true);
        
        owner->timers.push_back(timer_info);

        // Start timer thread
        std::thread timer_thread([timer_info]() {
            while (timer_info->active.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(timer_info->interval_ms));
                
                remidy::EventLoop::runTaskOnMainThread([timer_info]() {
                    if (!timer_info->active.load()) return;

                    auto handler = timer_info->handler;
                    if (handler) {
                        handler->onTimer();
                    }
                });
            }
        });
        timer_thread.detach();

        return kResultOk;
    }

    tresult PLUGIN_API HostApplication::RunLoopImpl::unregisterTimer(ITimerHandler* handler) {
        if (!owner || !handler)
            return kInvalidArgument;

        std::lock_guard<std::mutex> lock(owner->timers_mutex);
        
        auto it = owner->timers.begin();
        while (it != owner->timers.end()) {
            if ((*it)->handler == handler) {
                (*it)->active.store(false);
                it = owner->timers.erase(it);
                return kResultOk;
            } else {
                ++it;
            }
        }

        return kInvalidArgument;
    }
}
