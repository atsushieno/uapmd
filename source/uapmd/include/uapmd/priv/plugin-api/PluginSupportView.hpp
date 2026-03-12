#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "ParameterTypes.hpp"

namespace uapmd {

    enum class PerNoteContextFlags : uint32_t {
        None = 0,
        PerChannel = 1 << 0,
        PerNote = 1 << 1,
        PerGroup = 1 << 2,
        PerExtra = 1 << 3
    };

    inline constexpr PerNoteContextFlags operator|(PerNoteContextFlags lhs, PerNoteContextFlags rhs) {
        return static_cast<PerNoteContextFlags>(
            static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    inline constexpr PerNoteContextFlags operator&(PerNoteContextFlags lhs, PerNoteContextFlags rhs) {
        return static_cast<PerNoteContextFlags>(
            static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
    }

    struct PerNoteContext {
        uint32_t note{0};
        uint32_t channel{0};
        uint32_t group{0};
        uint32_t extra{0};
    };

    using ParameterListenerId = int64_t;

    enum class ParameterValueStatus : int32_t {
        Ok = 0,
        InvalidIndex = 1,
        Unsupported = 2,
        BackendError = 3
    };

    using ParameterChangeCallback = std::function<void(uint32_t, double)>;
    using ParameterMetadataCallback = std::function<void()>;
    using PerNoteControllerChangeCallback = std::function<void(PerNoteContextFlags, PerNoteContext, uint32_t, double)>;

    template<typename T>
    concept ParameterSupportLike = requires(
        T& support,
        const T& constSupport,
        ParameterChangeCallback valueCb,
        ParameterMetadataCallback metadataCb,
        PerNoteControllerChangeCallback perNoteCb,
        uint32_t index,
        double inValue,
        double& outValue,
        PerNoteContextFlags flags,
        PerNoteContext context,
        uint64_t timestamp) {
        { support.addParameterValueListener(std::move(valueCb)) } -> std::convertible_to<ParameterListenerId>;
        { support.removeParameterValueListener(ParameterListenerId{}) };
        { support.addParameterMetadataListener(std::move(metadataCb)) } -> std::convertible_to<ParameterListenerId>;
        { support.removeParameterMetadataListener(ParameterListenerId{}) };
        { support.addPerNoteControllerListener(std::move(perNoteCb)) } -> std::convertible_to<ParameterListenerId>;
        { support.removePerNoteControllerListener(ParameterListenerId{}) };
        { support.setParameterValue(index, inValue, timestamp) } -> std::same_as<ParameterValueStatus>;
        { support.getParameterValue(index, outValue) } -> std::same_as<ParameterValueStatus>;
        { support.setPerNoteControllerValue(context, index, inValue, timestamp) } -> std::same_as<ParameterValueStatus>;
        { support.getPerNoteControllerValue(context, index, outValue) } -> std::same_as<ParameterValueStatus>;
        { constSupport.normalizedParameterValue(index, inValue) } -> std::convertible_to<double>;
        { constSupport.normalizedPerNoteControllerValue(flags, context, index, inValue) } -> std::convertible_to<double>;
        { constSupport.perNoteControllerMetadata(flags, context) } -> std::same_as<std::vector<ParameterMetadata>>;
    };

    class ParameterSupportView {
    public:
        ParameterSupportView() = default;

        template<ParameterSupportLike T>
        explicit ParameterSupportView(std::shared_ptr<T> support)
            : impl_(std::make_shared<Model<T>>(std::move(support))) {}

        bool valid() const { return static_cast<bool>(impl_); }

        ParameterListenerId addParameterValueListener(ParameterChangeCallback cb) const {
            if (!impl_)
                return 0;
            return impl_->addParameterValueListener(std::move(cb));
        }

        void removeParameterValueListener(ParameterListenerId id) const {
            if (impl_)
                impl_->removeParameterValueListener(id);
        }

        ParameterListenerId addParameterMetadataListener(ParameterMetadataCallback cb) const {
            if (!impl_)
                return 0;
            return impl_->addParameterMetadataListener(std::move(cb));
        }

        void removeParameterMetadataListener(ParameterListenerId id) const {
            if (impl_)
                impl_->removeParameterMetadataListener(id);
        }

        ParameterListenerId addPerNoteControllerListener(PerNoteControllerChangeCallback cb) const {
            if (!impl_)
                return 0;
            return impl_->addPerNoteControllerListener(std::move(cb));
        }

        void removePerNoteControllerListener(ParameterListenerId id) const {
            if (impl_)
                impl_->removePerNoteControllerListener(id);
        }

        ParameterValueStatus setParameterValue(uint32_t index, double value, uint64_t timestamp = 0) const {
            if (!impl_)
                return ParameterValueStatus::BackendError;
            return impl_->setParameterValue(index, value, timestamp);
        }

        ParameterValueStatus getParameterValue(uint32_t index, double& value) const {
            if (!impl_)
                return ParameterValueStatus::BackendError;
            return impl_->getParameterValue(index, value);
        }

        ParameterValueStatus setPerNoteControllerValue(PerNoteContext context, uint32_t index, double value, uint64_t timestamp = 0) const {
            if (!impl_)
                return ParameterValueStatus::BackendError;
            return impl_->setPerNoteControllerValue(context, index, value, timestamp);
        }

        ParameterValueStatus getPerNoteControllerValue(PerNoteContext context, uint32_t index, double& value) const {
            if (!impl_)
                return ParameterValueStatus::BackendError;
            return impl_->getPerNoteControllerValue(context, index, value);
        }

        double normalizedParameterValue(uint32_t index, double plainValue) const {
            if (!impl_)
                return plainValue;
            return impl_->normalizedParameterValue(index, plainValue);
        }

        double normalizedPerNoteControllerValue(PerNoteContextFlags flags, PerNoteContext context, uint32_t index, double plainValue) const {
            if (!impl_)
                return plainValue;
            return impl_->normalizedPerNoteControllerValue(flags, context, index, plainValue);
        }

        std::vector<ParameterMetadata> perNoteControllerMetadata(PerNoteContextFlags flags, PerNoteContext context) const {
            if (!impl_)
                return {};
            return impl_->perNoteControllerMetadata(flags, context);
        }

    private:
        struct Concept {
            virtual ~Concept() = default;
            virtual ParameterListenerId addParameterValueListener(ParameterChangeCallback cb) = 0;
            virtual void removeParameterValueListener(ParameterListenerId id) = 0;
            virtual ParameterListenerId addParameterMetadataListener(ParameterMetadataCallback cb) = 0;
            virtual void removeParameterMetadataListener(ParameterListenerId id) = 0;
            virtual ParameterListenerId addPerNoteControllerListener(PerNoteControllerChangeCallback cb) = 0;
            virtual void removePerNoteControllerListener(ParameterListenerId id) = 0;
            virtual ParameterValueStatus setParameterValue(uint32_t index, double value, uint64_t timestamp) = 0;
            virtual ParameterValueStatus getParameterValue(uint32_t index, double& value) const = 0;
            virtual ParameterValueStatus setPerNoteControllerValue(PerNoteContext context, uint32_t index, double value, uint64_t timestamp) = 0;
            virtual ParameterValueStatus getPerNoteControllerValue(PerNoteContext context, uint32_t index, double& value) const = 0;
            virtual double normalizedParameterValue(uint32_t index, double plainValue) const = 0;
            virtual double normalizedPerNoteControllerValue(PerNoteContextFlags flags, PerNoteContext context, uint32_t index, double plainValue) const = 0;
            virtual std::vector<ParameterMetadata> perNoteControllerMetadata(PerNoteContextFlags flags, PerNoteContext context) const = 0;
        };

        template<typename T>
        struct Model final : Concept {
            explicit Model(std::shared_ptr<T> support) : support_(std::move(support)) {}

            ParameterListenerId addParameterValueListener(ParameterChangeCallback cb) override {
                return support_->addParameterValueListener(std::move(cb));
            }

            void removeParameterValueListener(ParameterListenerId id) override {
                support_->removeParameterValueListener(id);
            }

            ParameterListenerId addParameterMetadataListener(ParameterMetadataCallback cb) override {
                return support_->addParameterMetadataListener(std::move(cb));
            }

            void removeParameterMetadataListener(ParameterListenerId id) override {
                support_->removeParameterMetadataListener(id);
            }

            ParameterListenerId addPerNoteControllerListener(PerNoteControllerChangeCallback cb) override {
                return support_->addPerNoteControllerListener(std::move(cb));
            }

            void removePerNoteControllerListener(ParameterListenerId id) override {
                support_->removePerNoteControllerListener(id);
            }

            ParameterValueStatus setParameterValue(uint32_t index, double value, uint64_t timestamp) override {
                return support_->setParameterValue(index, value, timestamp);
            }

            ParameterValueStatus getParameterValue(uint32_t index, double& value) const override {
                return support_->getParameterValue(index, value);
            }

            ParameterValueStatus setPerNoteControllerValue(PerNoteContext context, uint32_t index, double value, uint64_t timestamp) override {
                return support_->setPerNoteControllerValue(context, index, value, timestamp);
            }

            ParameterValueStatus getPerNoteControllerValue(PerNoteContext context, uint32_t index, double& value) const override {
                return support_->getPerNoteControllerValue(context, index, value);
            }

            double normalizedParameterValue(uint32_t index, double plainValue) const override {
                return support_->normalizedParameterValue(index, plainValue);
            }

            double normalizedPerNoteControllerValue(PerNoteContextFlags flags, PerNoteContext context, uint32_t index, double plainValue) const override {
                return support_->normalizedPerNoteControllerValue(flags, context, index, plainValue);
            }

            std::vector<ParameterMetadata> perNoteControllerMetadata(PerNoteContextFlags flags, PerNoteContext context) const override {
                return support_->perNoteControllerMetadata(flags, context);
            }

            std::shared_ptr<T> support_;
        };

        std::shared_ptr<Concept> impl_;
    };

    enum class AudioBusRole {
        Main,
        Aux
    };

    struct AudioBusDescriptor {
        AudioBusRole role{AudioBusRole::Aux};
        uint32_t channels{0};
        bool enabled{false};
    };

    using AudioBusList = std::vector<AudioBusDescriptor>;

    template<typename T>
    concept AudioBusesLike = requires(const T& buses) {
        { buses.hasEventInputs() } -> std::convertible_to<bool>;
        { buses.hasEventOutputs() } -> std::convertible_to<bool>;
        { buses.audioInputBuses() } -> std::same_as<AudioBusList>;
        { buses.audioOutputBuses() } -> std::same_as<AudioBusList>;
        { buses.mainInputBusIndex() } -> std::convertible_to<int32_t>;
        { buses.mainOutputBusIndex() } -> std::convertible_to<int32_t>;
    };

    class AudioBusesView {
    public:
        AudioBusesView() = default;

        template<AudioBusesLike T>
        explicit AudioBusesView(std::shared_ptr<T> buses)
            : impl_(std::make_shared<Model<T>>(std::move(buses))) {}

        bool valid() const { return static_cast<bool>(impl_); }

        bool hasEventInputs() const {
            return impl_ ? impl_->hasEventInputs() : false;
        }

        bool hasEventOutputs() const {
            return impl_ ? impl_->hasEventOutputs() : false;
        }

        AudioBusList audioInputBuses() const {
            if (!impl_)
                return {};
            return impl_->audioInputBuses();
        }

        AudioBusList audioOutputBuses() const {
            if (!impl_)
                return {};
            return impl_->audioOutputBuses();
        }

        int32_t mainInputBusIndex() const {
            if (!impl_)
                return -1;
            return impl_->mainInputBusIndex();
        }

        int32_t mainOutputBusIndex() const {
            if (!impl_)
                return -1;
            return impl_->mainOutputBusIndex();
        }

    private:
        struct Concept {
            virtual ~Concept() = default;
            virtual bool hasEventInputs() const = 0;
            virtual bool hasEventOutputs() const = 0;
            virtual AudioBusList audioInputBuses() const = 0;
            virtual AudioBusList audioOutputBuses() const = 0;
            virtual int32_t mainInputBusIndex() const = 0;
            virtual int32_t mainOutputBusIndex() const = 0;
        };

        template<typename T>
        struct Model final : Concept {
            explicit Model(std::shared_ptr<T> buses) : buses_(std::move(buses)) {}

            bool hasEventInputs() const override { return buses_->hasEventInputs(); }
            bool hasEventOutputs() const override { return buses_->hasEventOutputs(); }
            AudioBusList audioInputBuses() const override { return buses_->audioInputBuses(); }
            AudioBusList audioOutputBuses() const override { return buses_->audioOutputBuses(); }
            int32_t mainInputBusIndex() const override { return buses_->mainInputBusIndex(); }
            int32_t mainOutputBusIndex() const override { return buses_->mainOutputBusIndex(); }

            std::shared_ptr<T> buses_;
        };

        std::shared_ptr<Concept> impl_;
    };

} // namespace uapmd
