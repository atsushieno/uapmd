#pragma once

#include "VST3Helper.hpp"
#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <format>
#include <atomic>
#include <mutex>
#include <memory>
#include <thread>
#include <chrono>

#if defined(__linux__) || defined(__unix__)
#ifdef HAVE_WAYLAND
#include <pluginterfaces/gui/iwaylandframe.h>
#endif
#endif

namespace remidy_vst3 {
    using namespace Steinberg::Vst;
    using namespace Steinberg::Linux;

    void logNoInterface(std::string label, const TUID _iid);

    // Host implementation
    class HostAttributeList : public IAttributeList {
    private:
        std::atomic<uint32_t> refCount{1};
        std::unordered_map<std::string, int64_t> intValues{};
        std::unordered_map<std::string, double> floatValues{};
        std::unordered_map<std::string, std::vector<int16_t>> stringValues{};
        std::unordered_map<std::string, std::vector<uint8_t>> binaryValues{};

    public:
        HostAttributeList() = default;
        virtual ~HostAttributeList() = default;

        // FUnknown interface
        tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE {
            QUERY_INTERFACE(_iid, obj, FUnknown::iid, IAttributeList)
            QUERY_INTERFACE(_iid, obj, IAttributeList::iid, IAttributeList)
            *obj = nullptr;
            return kNoInterface;
        }

        uint32 PLUGIN_API addRef() SMTG_OVERRIDE {
            return ++refCount;
        }

        uint32 PLUGIN_API release() SMTG_OVERRIDE {
            uint32 newCount = --refCount;
            if (newCount == 0) {
                delete this;
            }
            return newCount;
        }

        // IAttributeList interface
        tresult PLUGIN_API setInt(AttrID id, int64 value) SMTG_OVERRIDE {
            if (!id)
                return kInvalidArgument;
            intValues[id] = value;
            floatValues.erase(id);
            stringValues.erase(id);
            binaryValues.erase(id);
            return kResultOk;
        }

        tresult PLUGIN_API getInt(AttrID id, int64& value) SMTG_OVERRIDE {
            if (!id)
                return kInvalidArgument;
            auto it = intValues.find(id);
            if (it == intValues.end())
                return kResultFalse;
            value = it->second;
            return kResultOk;
        }

        tresult PLUGIN_API setFloat(AttrID id, double value) SMTG_OVERRIDE {
            if (!id)
                return kInvalidArgument;
            floatValues[id] = value;
            intValues.erase(id);
            stringValues.erase(id);
            binaryValues.erase(id);
            return kResultOk;
        }

        tresult PLUGIN_API getFloat(AttrID id, double& value) SMTG_OVERRIDE {
            if (!id)
                return kInvalidArgument;
            auto it = floatValues.find(id);
            if (it == floatValues.end())
                return kResultFalse;
            value = it->second;
            return kResultOk;
        }

        tresult PLUGIN_API setString(AttrID id, const TChar* string) SMTG_OVERRIDE {
            if (!id)
                return kInvalidArgument;
            std::vector<int16_t> data{};
            if (string) {
                const TChar* ptr = string;
                while (*ptr) {
                    data.push_back(*ptr++);
                }
            }
            data.push_back(0);
            stringValues[id] = std::move(data);
            intValues.erase(id);
            floatValues.erase(id);
            binaryValues.erase(id);
            return kResultOk;
        }

        tresult PLUGIN_API getString(AttrID id, TChar* string, uint32 sizeInBytes) SMTG_OVERRIDE {
            if (!id || !string || sizeInBytes == 0)
                return kInvalidArgument;
            auto it = stringValues.find(id);
            if (it == stringValues.end())
                return kResultFalse;
            const auto& data = it->second;
            const auto required = static_cast<uint32>(data.size() * sizeof(TChar));
            if (required > sizeInBytes)
                return kResultFalse;
            std::copy(data.begin(), data.end(), string);
            if (required < sizeInBytes)
                string[data.size()] = 0;
            return kResultOk;
        }

        tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32 sizeInBytes) SMTG_OVERRIDE {
            if (!id)
                return kInvalidArgument;
            std::vector<uint8_t> buffer{};
            if (data && sizeInBytes) {
                const uint8_t* ptr = static_cast<const uint8_t*>(data);
                buffer.assign(ptr, ptr + sizeInBytes);
            }
            binaryValues[id] = std::move(buffer);
            intValues.erase(id);
            floatValues.erase(id);
            stringValues.erase(id);
            return kResultOk;
        }

        tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32& sizeInBytes) SMTG_OVERRIDE {
            if (!id)
                return kInvalidArgument;
            auto it = binaryValues.find(id);
            if (it == binaryValues.end())
                return kResultFalse;
            const auto& buffer = it->second;
            data = buffer.empty() ? nullptr : buffer.data();
            sizeInBytes = static_cast<uint32>(buffer.size());
            return kResultOk;
        }

        auto asInterface() { return this; }
    };

    class HostMessage : public IMessage {
    private:
        std::atomic<uint32_t> refCount{1};
        std::string messageId;
        HostAttributeList* attributeList{};

    public:
        explicit HostMessage() {
            attributeList = new HostAttributeList();
        }

        virtual ~HostMessage() {
            if (attributeList)
                attributeList->release();
        }

        // FUnknown interface
        tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE {
            QUERY_INTERFACE(_iid, obj, FUnknown::iid, IMessage)
            QUERY_INTERFACE(_iid, obj, IMessage::iid, IMessage)
            *obj = nullptr;
            return kNoInterface;
        }

        uint32 PLUGIN_API addRef() SMTG_OVERRIDE {
            return ++refCount;
        }

        uint32 PLUGIN_API release() SMTG_OVERRIDE {
            uint32 newCount = --refCount;
            if (newCount == 0) {
                delete this;
            }
            return newCount;
        }

        // IMessage interface
        FIDString PLUGIN_API getMessageID() SMTG_OVERRIDE {
            return messageId.empty() ? nullptr : messageId.c_str();
        }

        void PLUGIN_API setMessageID(FIDString id) SMTG_OVERRIDE {
            messageId = id ? id : "";
        }

        IAttributeList* PLUGIN_API getAttributes() SMTG_OVERRIDE {
            return attributeList;
        }
    };

    class HostEventList : public IEventList {
    private:
        std::atomic<uint32_t> refCount{1};
        std::vector<Event> events{};

    public:
        explicit HostEventList() = default;
        virtual ~HostEventList() = default;

        // FUnknown interface
        tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE {
            QUERY_INTERFACE(_iid, obj, FUnknown::iid, IEventList)
            QUERY_INTERFACE(_iid, obj, IEventList::iid, IEventList)
            *obj = nullptr;
            return kNoInterface;
        }

        uint32 PLUGIN_API addRef() SMTG_OVERRIDE {
            return ++refCount;
        }

        uint32 PLUGIN_API release() SMTG_OVERRIDE {
            uint32 newCount = --refCount;
            if (newCount == 0) {
                delete this;
            }
            return newCount;
        }

        // IEventList interface
        int32 PLUGIN_API getEventCount() SMTG_OVERRIDE {
            return static_cast<int32>(events.size());
        }

        tresult PLUGIN_API getEvent(int32 index, Event& e) SMTG_OVERRIDE {
            if (index < 0 || index >= static_cast<int32>(events.size()))
                return kInvalidArgument;
            e = events[index];
            return kResultOk;
        }

        tresult PLUGIN_API addEvent(Event& e) SMTG_OVERRIDE {
            events.emplace_back(e);
            return kResultOk;
        }

        auto asInterface() { return this; }

        // invoked at process()
        void clear() {
            events.clear();
        }
    };

    class HostParamValueQueue : public IParamValueQueue {
    private:
        std::atomic<uint32_t> refCount{1};
        struct Point {
            int32 offset;
            ParamValue value;
        };
        const ParamID paramId;
        std::vector<Point> points{};

    public:
        explicit HostParamValueQueue(ParamID id) : paramId{id} {
            // FIXME: use some configured value
            points.reserve(1024);
        }

        virtual ~HostParamValueQueue() = default;

        // FUnknown interface
        tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE {
            QUERY_INTERFACE(_iid, obj, FUnknown::iid, IParamValueQueue)
            QUERY_INTERFACE(_iid, obj, IParamValueQueue::iid, IParamValueQueue)
            *obj = nullptr;
            return kNoInterface;
        }

        uint32 PLUGIN_API addRef() SMTG_OVERRIDE {
            return ++refCount;
        }

        uint32 PLUGIN_API release() SMTG_OVERRIDE {
            uint32 newCount = --refCount;
            if (newCount == 0) {
                delete this;
            }
            return newCount;
        }

        // IParamValueQueue interface
        ParamID PLUGIN_API getParameterId() SMTG_OVERRIDE {
            return paramId;
        }

        int32 PLUGIN_API getPointCount() SMTG_OVERRIDE {
            return static_cast<int32>(points.size());
        }

        tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& value) SMTG_OVERRIDE {
            if (index < 0 || index >= static_cast<int32>(points.size()))
                return kInvalidArgument;
            auto& p = points[index];
            sampleOffset = p.offset;
            value = p.value;
            return kResultOk;
        }

        tresult PLUGIN_API addPoint(int32 sampleOffset, ParamValue value, int32& index) SMTG_OVERRIDE {
            index = static_cast<int32>(points.size());
            Point pt{sampleOffset, value};
            points.emplace_back(pt);
            return kResultOk;
        }

        auto asInterface() { return this; }
    };

    class HostParameterChanges : public IParameterChanges {
    private:
        std::atomic<uint32_t> refCount{1};
        std::vector<std::unique_ptr<HostParamValueQueue>> queues{};

    public:
        explicit HostParameterChanges() = default;
        virtual ~HostParameterChanges() = default;

        // FUnknown interface
        tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE {
            QUERY_INTERFACE(_iid, obj, FUnknown::iid, IParameterChanges)
            QUERY_INTERFACE(_iid, obj, IParameterChanges::iid, IParameterChanges)
            *obj = nullptr;
            return kNoInterface;
        }

        uint32 PLUGIN_API addRef() SMTG_OVERRIDE {
            return ++refCount;
        }

        uint32 PLUGIN_API release() SMTG_OVERRIDE {
            uint32 newCount = --refCount;
            if (newCount == 0) {
                delete this;
            }
            return newCount;
        }

        // IParameterChanges interface
        int32 PLUGIN_API getParameterCount() SMTG_OVERRIDE {
            return static_cast<int32>(queues.size());
        }

        IParamValueQueue* PLUGIN_API getParameterData(int32 index) SMTG_OVERRIDE {
            if (index < 0 || index >= static_cast<int32>(queues.size()))
                return nullptr;
            auto& queue = queues[index];
            return queue ? queue->asInterface() : nullptr;
        }

        IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) SMTG_OVERRIDE {
            for (int32 i = 0; i < static_cast<int32>(queues.size()); ++i) {
                if (!queues[i])
                    continue;
                auto iface = queues[i]->asInterface();
                if (iface->getParameterId() == id) {
                    index = i;
                    return iface;
                }
            }
            index = static_cast<int32>(queues.size());
            // FIXME: this should not allocate. Move it elsewhere.
            queues.emplace_back(std::make_unique<HostParamValueQueue>(id));
            return queues[index]->asInterface();
        }

        IParameterChanges* asInterface() { return this; }

        void startProcessing() {
            // we need to allocate memory for IParamValueQueues for all the parameters.
            queues.clear();
        }

        void stopProcessing() {
            // we need to deallocate memory for IParamValueQueues for all the parameters.
            queues.clear();
        }

        // invoked at process()
        void clear() {
            // FIXME: this should not deallocate. Move it elsewhere.
            queues.clear();
        }
    };

    class VectorStream : public IBStream {
    private:
        std::atomic<uint32_t> refCount{1};
        std::vector<uint8_t>& data;
        int64 offset{0};

    public:
        explicit VectorStream(std::vector<uint8_t>& data) : data(data) {}
        virtual ~VectorStream() = default;

        // FUnknown interface
        tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE {
            QUERY_INTERFACE(_iid, obj, FUnknown::iid, IBStream)
            QUERY_INTERFACE(_iid, obj, IBStream::iid, IBStream)
            *obj = nullptr;
            return kNoInterface;
        }

        uint32 PLUGIN_API addRef() SMTG_OVERRIDE {
            return ++refCount;
        }

        uint32 PLUGIN_API release() SMTG_OVERRIDE {
            uint32 newCount = --refCount;
            if (newCount == 0) {
                delete this;
            }
            return newCount;
        }

        // IBStream interface
        tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead = nullptr) SMTG_OVERRIDE;
        tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten = nullptr) SMTG_OVERRIDE;
        tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result = nullptr) SMTG_OVERRIDE;
        tresult PLUGIN_API tell(int64* pos) SMTG_OVERRIDE;
    };

    class InterceptingConnectionPoint : public IConnectionPoint {
    private:
        std::atomic<uint32_t> refCount{1};
        IConnectionPoint* target{nullptr};

    public:
        explicit InterceptingConnectionPoint(IConnectionPoint* targetConnectionPoint)
            : target(targetConnectionPoint) {}

        virtual ~InterceptingConnectionPoint() = default;

        // FUnknown interface
        tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE {
            QUERY_INTERFACE(_iid, obj, FUnknown::iid, IConnectionPoint)
            QUERY_INTERFACE(_iid, obj, IConnectionPoint::iid, IConnectionPoint)
            // Forward to target if we don't support the interface
            if (target)
                return target->queryInterface(_iid, obj);
            *obj = nullptr;
            return kNoInterface;
        }

        uint32 PLUGIN_API addRef() SMTG_OVERRIDE {
            return ++refCount;
        }

        uint32 PLUGIN_API release() SMTG_OVERRIDE {
            uint32 newCount = --refCount;
            if (newCount == 0) {
                delete this;
            }
            return newCount;
        }

        // IConnectionPoint interface
        tresult PLUGIN_API connect(IConnectionPoint* other) SMTG_OVERRIDE {
            std::cerr << "InterceptingConnectionPoint::connect called with other=" << (void*)other << std::endl;
            if (!target)
                return kInvalidArgument;
            return target->connect(other);
        }

        tresult PLUGIN_API disconnect(IConnectionPoint* other) SMTG_OVERRIDE {
            std::cerr << "InterceptingConnectionPoint::disconnect called with other=" << (void*)other << std::endl;
            if (!target)
                return kInvalidArgument;
            return target->disconnect(other);
        }

        tresult PLUGIN_API notify(IMessage* message) SMTG_OVERRIDE {
            std::cerr << "InterceptingConnectionPoint::notify called with message=" << (void*)message << std::endl;
            if (!target)
                return kInvalidArgument;
            return target->notify(message);
        }
    };

    // HostApplication class implements IHostApplication and provides access to various
    // host-side interface implementations through nested classes
    class HostApplication : public IHostApplication {
    private:
        std::atomic<uint32_t> refCount{1};
        remidy::Logger* logger;

        // IRunLoop timer management
        struct TimerInfo {
            ITimerHandler* handler;
            uint64_t interval_ms;
            std::atomic<bool> active{true};
        };
        std::vector<std::shared_ptr<TimerInfo>> timers{};
        std::mutex timers_mutex{};

        // IRunLoop event handler management
        struct EventHandlerInfo {
            IEventHandler* handler;
            int fd;
            std::atomic<bool> active{true};
        };
        std::vector<std::shared_ptr<EventHandlerInfo>> event_handlers{};
        std::mutex event_handlers_mutex{};

        static const std::basic_string<char16_t> name16t;

        // Nested interface implementation classes
        struct EventHandlerImpl : public IEventHandler {
            std::atomic<uint32_t> refCount{1};
            HostApplication* owner;

            explicit EventHandlerImpl(HostApplication* owner) : owner(owner) {}
            virtual ~EventHandlerImpl() = default;

            tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE;
            uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refCount; }
            uint32 PLUGIN_API release() SMTG_OVERRIDE {
                uint32 newCount = --refCount;
                if (newCount == 0) delete this;
                return newCount;
            }
            void PLUGIN_API onFDIsSet(int fd) SMTG_OVERRIDE;
        };

        struct UnitHandlerImpl : public IUnitHandler {
            std::atomic<uint32_t> refCount{1};
            HostApplication* owner;

            explicit UnitHandlerImpl(HostApplication* owner) : owner(owner) {}
            virtual ~UnitHandlerImpl() = default;

            tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE;
            uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refCount; }
            uint32 PLUGIN_API release() SMTG_OVERRIDE {
                uint32 newCount = --refCount;
                if (newCount == 0) delete this;
                return newCount;
            }
            tresult PLUGIN_API notifyUnitSelection(UnitID unitId) SMTG_OVERRIDE;
            tresult PLUGIN_API notifyProgramListChange(ProgramListID listId, int32 programIndex) SMTG_OVERRIDE;
        };

        struct MessageImpl : public IMessage {
            std::atomic<uint32_t> refCount{1};
            HostApplication* owner;
            std::string message_id{};
            IAttributeList* attributes{nullptr};

            explicit MessageImpl(HostApplication* owner) : owner(owner) {
                attributes = new HostAttributeList();
            }
            virtual ~MessageImpl() {
                if (attributes) attributes->release();
            }

            tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE;
            uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refCount; }
            uint32 PLUGIN_API release() SMTG_OVERRIDE {
                uint32 newCount = --refCount;
                if (newCount == 0) delete this;
                return newCount;
            }
            FIDString PLUGIN_API getMessageID() SMTG_OVERRIDE;
            void PLUGIN_API setMessageID(FIDString id) SMTG_OVERRIDE;
            IAttributeList* PLUGIN_API getAttributes() SMTG_OVERRIDE;
        };

        struct PlugInterfaceSupportImpl : public IPlugInterfaceSupport {
            std::atomic<uint32_t> refCount{1};
            HostApplication* owner;

            explicit PlugInterfaceSupportImpl(HostApplication* owner) : owner(owner) {}
            virtual ~PlugInterfaceSupportImpl() = default;

            tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE;
            uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refCount; }
            uint32 PLUGIN_API release() SMTG_OVERRIDE {
                uint32 newCount = --refCount;
                if (newCount == 0) delete this;
                return newCount;
            }
            tresult PLUGIN_API isPlugInterfaceSupported(const TUID _iid) SMTG_OVERRIDE;
        };

#ifdef HAVE_WAYLAND
        struct WaylandHostImpl : public IWaylandHost {
            std::atomic<uint32_t> refCount{1};
            HostApplication* owner;

            explicit WaylandHostImpl(HostApplication* owner) : owner(owner) {}
            virtual ~WaylandHostImpl() = default;

            tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE;
            uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refCount; }
            uint32 PLUGIN_API release() SMTG_OVERRIDE {
                uint32 newCount = --refCount;
                if (newCount == 0) delete this;
                return newCount;
            }

            wl_display* PLUGIN_API openWaylandConnection() SMTG_OVERRIDE;
            tresult PLUGIN_API closeWaylandConnection(wl_display* display) SMTG_OVERRIDE;
        };
#endif

        struct RunLoopImpl : public IRunLoop {
            std::atomic<uint32_t> refCount{1};
            HostApplication* owner;

            explicit RunLoopImpl(HostApplication* owner) : owner(owner) {}
            virtual ~RunLoopImpl() = default;

            tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE;
            uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refCount; }
            uint32 PLUGIN_API release() SMTG_OVERRIDE {
                uint32 newCount = --refCount;
                if (newCount == 0) delete this;
                return newCount;
            }
            tresult PLUGIN_API registerEventHandler(IEventHandler* handler, FileDescriptor fd) SMTG_OVERRIDE;
            tresult PLUGIN_API unregisterEventHandler(IEventHandler* handler) SMTG_OVERRIDE;
            tresult PLUGIN_API registerTimer(ITimerHandler* handler, TimerInterval milliseconds) SMTG_OVERRIDE;
            tresult PLUGIN_API unregisterTimer(ITimerHandler* handler) SMTG_OVERRIDE;
        };

        EventHandlerImpl* event_handler{nullptr};
        UnitHandlerImpl* unit_handler{nullptr};
        PlugInterfaceSupportImpl* support{nullptr};
        RunLoopImpl* run_loop{nullptr};
#ifdef HAVE_WAYLAND
        WaylandHostImpl* wayland_host{nullptr};
#endif
        HostParameterChanges parameter_changes{};

    public:
        explicit HostApplication(remidy::Logger* logger);
        virtual ~HostApplication();

        // FUnknown interface
        tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE;
        uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refCount; }
        uint32 PLUGIN_API release() SMTG_OVERRIDE {
            uint32 newCount = --refCount;
            if (newCount == 0) delete this;
            return newCount;
        }

        // IHostApplication interface
        tresult PLUGIN_API getName(String128 name) SMTG_OVERRIDE;
        tresult PLUGIN_API createInstance(TUID cid, TUID _iid, void** obj) SMTG_OVERRIDE;

        inline IUnitHandler* getUnitHandler() { return unit_handler; }
        inline IPlugInterfaceSupport* getPlugInterfaceSupport() { return support; }
        inline IRunLoop* getRunLoop() { return run_loop; }
#ifdef HAVE_WAYLAND
        inline IWaylandHost* getWaylandHost() { return wayland_host; }
#endif

        void startProcessing();
        void stopProcessing();

    };
}
