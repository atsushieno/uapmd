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

#if defined(_MSC_VER)
#define min(v1, v2) (v1 < v2 ? v1 : v2)
#else
#define min(v1, v2) std::min(v1, v2)
#endif

namespace remidy_vst3 {
    using namespace Steinberg::Vst;
    using namespace Steinberg::Linux;

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
        tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead = nullptr) SMTG_OVERRIDE {
            auto size = min(static_cast<int32>(data.size() - offset), numBytes);
            if (size < numBytes) {
                // it is impossible for the input stream to complete reading
                return kInvalidArgument;
            }
            memcpy(buffer, data.data() + offset, size);
            offset += size;
            if (numBytesRead)
                *numBytesRead = size;
            return kResultOk;
        }

        tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten = nullptr) SMTG_OVERRIDE {
            data.resize(data.size() + numBytes);
            auto size = min(static_cast<int32>(data.size() - offset), numBytes);
            memcpy(data.data() + offset, buffer, size);
            offset += size;
            if (numBytesWritten)
                *numBytesWritten = size;
            return kResultOk;
        }

        tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result = nullptr) SMTG_OVERRIDE {
            switch (mode) {
                case IBStream::kIBSeekSet:
                    if (pos >= static_cast<int64>(data.size()))
                        return kInvalidArgument;
                    offset = pos;
                    break;
                case IBStream::kIBSeekCur:
                    if (offset + pos >= static_cast<int64>(data.size()))
                        return kInvalidArgument;
                    offset += pos;
                    break;
                case IBStream::kIBSeekEnd:
                    if (pos > static_cast<int64>(data.size()))
                        return kInvalidArgument;
                    offset = data.size() - pos;
                    break;
            }
            if (result)
                *result = offset;
            return kResultOk;
        }

        tresult PLUGIN_API tell(int64* pos) SMTG_OVERRIDE {
            if (pos)
                *pos = offset;
            return kResultOk;
        }
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
        std::unordered_map<void*, std::function<bool(uint32_t, uint32_t)>> resize_request_handlers{};
        std::unordered_map<void*, std::function<void(ParamID, double)>> parameter_edit_handlers{};

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

        struct ComponentHandlerImpl : public IComponentHandler {
            std::atomic<uint32_t> refCount{1};
            HostApplication* owner;

            explicit ComponentHandlerImpl(HostApplication* owner) : owner(owner) {}
            virtual ~ComponentHandlerImpl() = default;

            tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE;
            uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refCount; }
            uint32 PLUGIN_API release() SMTG_OVERRIDE {
                uint32 newCount = --refCount;
                if (newCount == 0) delete this;
                return newCount;
            }
            tresult PLUGIN_API beginEdit(ParamID id) SMTG_OVERRIDE;
            tresult PLUGIN_API performEdit(ParamID id, ParamValue valueNormalized) SMTG_OVERRIDE;
            tresult PLUGIN_API endEdit(ParamID id) SMTG_OVERRIDE;
            tresult PLUGIN_API restartComponent(int32 flags) SMTG_OVERRIDE;
        };

        struct ComponentHandler2Impl : public IComponentHandler2 {
            std::atomic<uint32_t> refCount{1};
            HostApplication* owner;

            explicit ComponentHandler2Impl(HostApplication* owner) : owner(owner) {}
            virtual ~ComponentHandler2Impl() = default;

            tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE;
            uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refCount; }
            uint32 PLUGIN_API release() SMTG_OVERRIDE {
                uint32 newCount = --refCount;
                if (newCount == 0) delete this;
                return newCount;
            }
            // IComponentHandler methods (inherited from IComponentHandler2's base)
            tresult PLUGIN_API beginEdit(ParamID id) { return owner->handler->beginEdit(id); }
            tresult PLUGIN_API performEdit(ParamID id, ParamValue valueNormalized) { return owner->handler->performEdit(id, valueNormalized); }
            tresult PLUGIN_API endEdit(ParamID id) { return owner->handler->endEdit(id); }
            tresult PLUGIN_API restartComponent(int32 flags) { return owner->handler->restartComponent(flags); }
            // IComponentHandler2-specific methods
            tresult PLUGIN_API setDirty(TBool state) SMTG_OVERRIDE;
            tresult PLUGIN_API requestOpenEditor(FIDString name = ViewType::kEditor) SMTG_OVERRIDE;
            tresult PLUGIN_API startGroupEdit() SMTG_OVERRIDE;
            tresult PLUGIN_API finishGroupEdit() SMTG_OVERRIDE;
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

        struct PlugFrameImpl : public IPlugFrame {
            std::atomic<uint32_t> refCount{1};
            HostApplication* owner;

            explicit PlugFrameImpl(HostApplication* owner) : owner(owner) {}
            virtual ~PlugFrameImpl() = default;

            tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE;
            uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refCount; }
            uint32 PLUGIN_API release() SMTG_OVERRIDE {
                uint32 newCount = --refCount;
                if (newCount == 0) delete this;
                return newCount;
            }
            tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) SMTG_OVERRIDE;
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
        ComponentHandlerImpl* handler{nullptr};
        ComponentHandler2Impl* handler2{nullptr};
        UnitHandlerImpl* unit_handler{nullptr};
        MessageImpl* message{nullptr};
        PlugFrameImpl* plug_frame{nullptr};
        PlugInterfaceSupportImpl* support{nullptr};
        RunLoopImpl* run_loop{nullptr};
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

        inline IComponentHandler* getComponentHandler() { return handler; }
        inline IComponentHandler2* getComponentHandler2() { return handler2; }
        inline IUnitHandler* getUnitHandler() { return unit_handler; }
        inline IPlugInterfaceSupport* getPlugInterfaceSupport() { return support; }
        inline IRunLoop* getRunLoop() { return run_loop; }
        inline IPlugFrame* getPlugFrame() { return plug_frame; }

        void startProcessing();
        void stopProcessing();

        void setResizeRequestHandler(void* view, std::function<bool(uint32_t, uint32_t)> handler_func) {
            if (handler_func)
                resize_request_handlers[view] = std::move(handler_func);
            else
                resize_request_handlers.erase(view);
        }

        void setParameterEditHandler(void* controller, std::function<void(ParamID, double)> handler_func) {
            if (handler_func)
                parameter_edit_handlers[controller] = std::move(handler_func);
            else
                parameter_edit_handlers.erase(controller);
        }

    };
}
