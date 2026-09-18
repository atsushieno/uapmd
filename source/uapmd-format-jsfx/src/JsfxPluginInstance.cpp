#include "JsfxPluginInstance.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "JsfxFramebufferUI.hpp"
#include "JsfxScanning.hpp"
#include "JsfxStateCodec.hpp"

namespace uapmd_jsfx {

    namespace {
        // ysfx reports script diagnostics -- undeclared variables, odd slider declarations
        // and the like -- through a reporter that writes to stderr when none is set. Those
        // are about someone else's script, so they belong in the application's log next to
        // every other plugin's complaints rather than on the console.
        void reportYsfxLog(intptr_t, ysfx_log_level level, const char* message) {
            if (!message)
                return;
            auto* logger = remidy::Logger::global();
            if (!logger)
                return;
            switch (level) {
                case ysfx_log_error: logger->logError("[JSFX] %s", message); break;
                case ysfx_log_warning: logger->logWarning("[JSFX] %s", message); break;
                default: logger->logInfo("[JSFX] %s", message); break;
            }
        }

        remidy::AudioChannelLayout layoutForChannels(uint32_t channels) {
            switch (channels) {
                case 1: return remidy::AudioChannelLayout::mono();
                case 2: return remidy::AudioChannelLayout::stereo();
                default: return remidy::AudioChannelLayout{std::to_string(channels) + " channels", channels};
            }
        }
    }

    JsfxAudioBuses::JsfxAudioBuses(uint32_t inputChannels, uint32_t outputChannels) {
        // The definitions have to outlive the configurations that reference them, and
        // AudioBusConfiguration takes its definition by non-const reference, so both live
        // here and neither vector is allowed to reallocate afterwards.
        definitions_.reserve(2);
        owned_.reserve(2);

        if (inputChannels > 0) {
            definitions_.emplace_back("Input", remidy::AudioBusRole::Main,
                                      std::vector<remidy::AudioChannelLayout>{layoutForChannels(inputChannels)});
            auto config = std::make_unique<remidy::AudioBusConfiguration>(definitions_.back());
            config->channelLayout(layoutForChannels(inputChannels));
            inputs_.emplace_back(config.get());
            owned_.emplace_back(std::move(config));
        }
        if (outputChannels > 0) {
            definitions_.emplace_back("Output", remidy::AudioBusRole::Main,
                                      std::vector<remidy::AudioChannelLayout>{layoutForChannels(outputChannels)});
            auto config = std::make_unique<remidy::AudioBusConfiguration>(definitions_.back());
            config->channelLayout(layoutForChannels(outputChannels));
            outputs_.emplace_back(config.get());
            owned_.emplace_back(std::move(config));
        }
    }

    JsfxParameterSupport::JsfxParameterSupport(ysfx_t* fx) : fx_(fx) {
        rebuild();
    }

    void JsfxParameterSupport::rebuild() {
        owned_.clear();
        parameters_.clear();
        slider_indices_.clear();
        if (!fx_)
            return;

        for (uint32_t slider = 0; slider < ysfx_max_sliders; slider++) {
            if (!ysfx_slider_exists(fx_, slider))
                continue;

            ysfx_slider_range_t range{};
            if (!ysfx_slider_get_range(fx_, slider, &range))
                continue;

            const char* name = ysfx_slider_get_name(fx_, slider);
            const uint32_t index = static_cast<uint32_t>(parameters_.size());

            // Sliders are written as slider1..sliderN in the script, so the stable id
            // follows the script's own numbering rather than our dense index.
            std::string stableId = "slider" + std::to_string(slider + 1);
            std::string displayName = name && *name ? std::string{name} : stableId;
            std::string path{};

            std::vector<remidy::ParameterEnumeration> enums{};
            const bool isEnum = ysfx_slider_is_enum(fx_, slider);
            if (isEnum) {
                const uint32_t count = ysfx_slider_get_enum_names(fx_, slider, nullptr, 0);
                for (uint32_t i = 0; i < count; i++) {
                    const char* label = ysfx_slider_get_enum_name(fx_, slider, i);
                    std::string labelText = label ? label : std::to_string(i);
                    enums.emplace_back(labelText, static_cast<double>(i));
                }
            }

            // A slider is discrete when it enumerates its values, or when it steps in whole
            // numbers. JSFX writes the latter as `slider1:0<0,4,1>` and expects a stepped
            // control rather than a continuous one.
            const bool integralStep = range.inc >= 1.0 && std::fabs(range.inc - std::round(range.inc)) < 1e-9;

            owned_.emplace_back(std::make_unique<remidy::PluginParameter>(
                    index, stableId, displayName, path,
                    static_cast<double>(range.def),
                    static_cast<double>(range.min),
                    static_cast<double>(range.max),
                    true, true,
                    !ysfx_slider_is_initially_visible(fx_, slider),
                    isEnum || integralStep,
                    std::move(enums)));
            parameters_.emplace_back(owned_.back().get());
            slider_indices_.emplace_back(slider);
        }
    }

    bool JsfxParameterSupport::sliderForIndex(uint32_t index, uint32_t& slider) const {
        if (index >= slider_indices_.size())
            return false;
        slider = slider_indices_[index];
        return true;
    }

    remidy::StatusCode JsfxParameterSupport::setParameter(uint32_t index, double plainValue) {
        uint32_t slider{};
        if (!fx_ || !sliderForIndex(index, slider))
            return remidy::StatusCode::INVALID_PARAMETER_OPERATION;
        // ysfx has no lock between slider writes and audio processing; writing a slider is
        // a double store plus a change flag, and racing them is how JSFX has always worked.
        ysfx_slider_set_value(fx_, slider, static_cast<ysfx_real>(plainValue), true);
        return remidy::StatusCode::OK;
    }

    remidy::StatusCode JsfxParameterSupport::enqueueParameterRT(uint32_t index, double plainValue,
                                                                uint64_t timestamp) {
        // This is called from the audio thread, so the value is applied where it is asked
        // for rather than queued. There is nothing to defer: the write is two stores.
        (void) timestamp;
        return setParameter(index, plainValue);
    }

    remidy::StatusCode JsfxParameterSupport::getParameter(uint32_t index, double* plainValue) {
        uint32_t slider{};
        if (!fx_ || !plainValue || !sliderForIndex(index, slider))
            return remidy::StatusCode::INVALID_PARAMETER_OPERATION;
        *plainValue = static_cast<double>(ysfx_slider_get_value(fx_, slider));
        return remidy::StatusCode::OK;
    }

    remidy::StatusCode JsfxParameterSupport::setPerNoteController(remidy::PerNoteControllerContext context,
                                                                  uint32_t index, double value) {
        (void) context; (void) index; (void) value;
        return remidy::StatusCode::INVALID_PARAMETER_OPERATION;
    }

    remidy::StatusCode JsfxParameterSupport::enqueuePerNoteControllerRT(remidy::PerNoteControllerContext context,
                                                                        uint32_t index, double value,
                                                                        uint64_t timestamp) {
        (void) context; (void) index; (void) value; (void) timestamp;
        return remidy::StatusCode::INVALID_PARAMETER_OPERATION;
    }

    remidy::StatusCode JsfxParameterSupport::getPerNoteController(remidy::PerNoteControllerContext context,
                                                                  uint32_t index, double* value) {
        (void) context; (void) index; (void) value;
        return remidy::StatusCode::INVALID_PARAMETER_OPERATION;
    }

    std::string JsfxParameterSupport::valueToString(uint32_t index, double value) {
        if (index < parameters_.size()) {
            auto* parameter = parameters_[index];
            for (auto& entry : parameter->enums())
                if (std::fabs(entry.value - value) < 1e-9)
                    return entry.label;
        }
        // JSFX has no value formatting callback, so the number is all there is. The unit,
        // when a script names one, is already part of the slider name.
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%g", value);
        return buffer;
    }

    std::string JsfxParameterSupport::valueToStringPerNote(remidy::PerNoteControllerContext context,
                                                           uint32_t index, double value) {
        (void) context; (void) index; (void) value;
        return {};
    }

    void JsfxParameterSupport::refreshParameterMetadata(uint32_t index) {
        uint32_t slider{};
        if (!fx_ || index >= parameters_.size() || !sliderForIndex(index, slider))
            return;
        ysfx_slider_range_t range{};
        if (!ysfx_slider_get_range(fx_, slider, &range))
            return;
        // A script may move a slider's range while it runs, which is what this exists for.
        parameters_[index]->updateRange(static_cast<double>(range.min),
                                        static_cast<double>(range.max),
                                        static_cast<double>(range.def));
    }

    // Turns the host's UMP stream into the MIDI 1.0 byte messages JSFX understands.
    //
    // JSFX predates MIDI 2.0 and its midirecv() only ever sees three-byte messages, so the
    // conversion is lossy in the expected direction: 16-bit velocity is truncated to 7 bits
    // and per-note controllers have nowhere to go.
    class JsfxMidiDispatcher : public remidy::TypedUmpInputDispatcher {
        ysfx_t* fx_;

        void send(uint8_t status, uint8_t data1, uint8_t data2) {
            uint8_t bytes[3] = {status, data1, data2};
            ysfx_midi_event_t event{};
            event.bus = 0;
            event.offset = static_cast<uint32_t>(timestamp());
            event.size = sizeof(bytes);
            event.data = bytes;
            ysfx_send_midi(fx_, &event);
        }

    protected:
        void onNoteOn(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t attributeType,
                      uint16_t velocity, uint16_t attribute) override {
            (void) group; (void) attributeType; (void) attribute;
            send(0x90 | (channel & 0xF), note & 0x7F, static_cast<uint8_t>(velocity >> 9));
        }
        void onNoteOff(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t attributeType,
                       uint16_t velocity, uint16_t attribute) override {
            (void) group; (void) attributeType; (void) attribute;
            send(0x80 | (channel & 0xF), note & 0x7F, static_cast<uint8_t>(velocity >> 9));
        }
        void onCC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t index, uint32_t data) override {
            (void) group;
            send(0xB0 | (channel & 0xF), index & 0x7F, static_cast<uint8_t>(data >> 25));
        }
        void onProgramChange(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t flags, remidy::uint7_t program,
                             remidy::uint7_t bankMSB, remidy::uint7_t bankLSB) override {
            (void) group;
            if (flags & 1) {
                send(0xB0 | (channel & 0xF), 0x00, bankMSB & 0x7F);
                send(0xB0 | (channel & 0xF), 0x20, bankLSB & 0x7F);
            }
            send(0xC0 | (channel & 0xF), program & 0x7F, 0);
        }
        void onPitchBend(remidy::uint4_t group, remidy::uint4_t channel, int8_t perNoteOrMinus, uint32_t data) override {
            (void) group;
            // Per-note pitch bend has no MIDI 1.0 equivalent; only the channel-wide form
            // can be delivered.
            if (perNoteOrMinus >= 0)
                return;
            const uint16_t bend14 = static_cast<uint16_t>(data >> 18);
            send(0xE0 | (channel & 0xF), bend14 & 0x7F, static_cast<uint8_t>((bend14 >> 7) & 0x7F));
        }
        void onPressure(remidy::uint4_t group, remidy::uint4_t channel, int8_t perNoteOrMinus, uint32_t data) override {
            (void) group;
            if (perNoteOrMinus >= 0)
                send(0xA0 | (channel & 0xF), static_cast<uint8_t>(perNoteOrMinus) & 0x7F,
                     static_cast<uint8_t>(data >> 25));
            else
                send(0xD0 | (channel & 0xF), static_cast<uint8_t>(data >> 25), 0);
        }

    public:
        explicit JsfxMidiDispatcher(ysfx_t* fx) : fx_(fx) {}
    };

    struct JsfxPluginInstance::Impl {
        ysfx_config_u config{};
        ysfx_u fx{};
        std::unique_ptr<JsfxAudioBuses> buses{};
        std::unique_ptr<JsfxParameterSupport> parameters{};
        std::unique_ptr<JsfxMidiDispatcher> midi{};
        // Declared after `fx` so it is destroyed before it: the editor holds a worker that
        // may be inside ysfx, and `ysfx_unload_code` wants the same lock that worker holds.
        std::unique_ptr<JsfxFramebufferUI> ui{};
        bool ui_visible{false};
        std::string format_name{kFormatName};
        uint32_t block_size{0};
        std::vector<const float*> input_pointers{};
        std::vector<float*> output_pointers{};
        bool processing{false};
    };

    JsfxPluginInstance::JsfxPluginInstance(std::unique_ptr<Impl> impl, const std::string& displayName,
                                           const std::string& pluginId) :
            AudioPluginInstanceBase(displayName, kFormatName, pluginId),
            impl_(std::move(impl)) {
    }

    JsfxPluginInstance::~JsfxPluginInstance() = default;

    std::unique_ptr<JsfxPluginInstance> JsfxPluginInstance::create(
            const std::filesystem::path& path,
            const std::string& pluginId,
            const uapmd_plugin_hosting::AudioPluginInstantiationOptions& options,
            std::string& error) {
        auto impl = std::make_unique<Impl>();

        impl->config.reset(ysfx_config_new());
        if (!impl->config) {
            error = "could not create the JSFX configuration";
            return {};
        }
        ysfx_set_log_reporter(impl->config.get(), &reportYsfxLog);
        // WAV and FLAC reading is what a script's file_open() and sample loading rely on.
        ysfx_register_builtin_audio_formats(impl->config.get());
        // Resolves the effect's import root and data root by finding the Effects/ and Data/
        // directories above it. Without this, scripts that load impulse responses or
        // samples fail at runtime rather than at load.
        ysfx_guess_file_roots(impl->config.get(), path.string().c_str());

        impl->fx.reset(ysfx_new(impl->config.get()));
        if (!impl->fx) {
            error = "could not create the JSFX instance";
            return {};
        }

        if (!ysfx_load_file(impl->fx.get(), path.string().c_str(), 0)) {
            error = "could not load " + path.string();
            return {};
        }
        if (!ysfx_compile(impl->fx.get(), 0)) {
            error = "could not compile " + path.string();
            return {};
        }

        ysfx_set_sample_rate(impl->fx.get(), static_cast<ysfx_real>(options.sampleRate));
        ysfx_set_block_size(impl->fx.get(), options.bufferSizeInSamples);
        ysfx_init(impl->fx.get());

        impl->block_size = options.bufferSizeInSamples;
        impl->buses = std::make_unique<JsfxAudioBuses>(ysfx_get_num_inputs(impl->fx.get()),
                                                       ysfx_get_num_outputs(impl->fx.get()));
        impl->parameters = std::make_unique<JsfxParameterSupport>(impl->fx.get());
        impl->midi = std::make_unique<JsfxMidiDispatcher>(impl->fx.get());
#if UAPMD_JSFX_HAS_GFX
        // A script without a @gfx section has no editor, and neither does any script on a
        // build without graphics support: ysfx still parses the section, but @gfx never
        // runs, so offering an editor would offer a window that stays blank.
        if (ysfx_has_section(impl->fx.get(), ysfx_section_gfx))
            impl->ui = std::make_unique<JsfxFramebufferUI>(impl->fx.get());
#endif

        const char* name = ysfx_get_name(impl->fx.get());
        std::string displayName = name && *name ? std::string{name} : path.filename().string();

        auto* fx = impl->fx.get();
        (void) fx;
        return std::unique_ptr<JsfxPluginInstance>(
                new JsfxPluginInstance(std::move(impl), displayName, pluginId));
    }

    uapmd_status_t JsfxPluginInstance::startProcessing() {
        impl_->processing = true;
        return 0;
    }

    uapmd_status_t JsfxPluginInstance::stopProcessing() {
        impl_->processing = false;
        return 0;
    }

    uint32_t JsfxPluginInstance::latencyInSamples() const {
        if (!impl_->fx)
            return 0;
        const auto delay = ysfx_get_pdc_delay(impl_->fx.get());
        return delay > 0 ? static_cast<uint32_t>(delay) : 0;
    }

    uapmd_status_t JsfxPluginInstance::processAudio(remidy::AudioProcessContext& process) {
        auto* fx = impl_->fx.get();
        if (!fx)
            return 1;

        const uint32_t frames = process.frameCount();
        if (frames == 0)
            return 0;

        // A block larger than the effect was initialised for would overrun the buffers it
        // allocated in @init, so tell it before anything else happens.
        if (frames > impl_->block_size) {
            ysfx_set_block_size(fx, frames);
            impl_->block_size = frames;
        }

        auto& master = process.masterContext();
        ysfx_time_info_t timing{};
        timing.tempo = static_cast<ysfx_real>(60000000.0 / static_cast<double>(master.tempo()));
        timing.playback_state = master.isPlaying() ? ysfx_playback_playing : ysfx_playback_stopped;
        timing.time_position =
                static_cast<ysfx_real>(static_cast<double>(master.playbackPositionSamples()) / master.sampleRate());
        timing.beat_position = static_cast<ysfx_real>(master.ppqPosition());
        timing.time_signature[0] = static_cast<uint32_t>(master.timeSignatureNumerator());
        timing.time_signature[1] = static_cast<uint32_t>(master.timeSignatureDenominator());
        ysfx_set_time_info(fx, &timing);

        // Events first: JSFX expects everything for the cycle to be queued before it runs.
        impl_->midi->process(process);

        const int32_t inBus = impl_->buses->mainInputBusIndex();
        const int32_t outBus = impl_->buses->mainOutputBusIndex();
        const uint32_t inChannels = inBus < 0 ? 0 : process.inputChannelCount(static_cast<uint32_t>(inBus));
        const uint32_t outChannels = outBus < 0 ? 0 : process.outputChannelCount(static_cast<uint32_t>(outBus));

        impl_->input_pointers.clear();
        impl_->output_pointers.clear();
        for (uint32_t ch = 0; ch < inChannels; ch++)
            impl_->input_pointers.emplace_back(process.getFloatInBuffer(static_cast<uint32_t>(inBus), ch));
        for (uint32_t ch = 0; ch < outChannels; ch++)
            impl_->output_pointers.emplace_back(process.getFloatOutBuffer(static_cast<uint32_t>(outBus), ch));

        ysfx_process_float(fx,
                           impl_->input_pointers.empty() ? nullptr : impl_->input_pointers.data(),
                           impl_->output_pointers.empty() ? nullptr : impl_->output_pointers.data(),
                           static_cast<uint32_t>(impl_->input_pointers.size()),
                           static_cast<uint32_t>(impl_->output_pointers.size()),
                           frames);

        collectMidiOutput(process);
        return 0;
    }

    void JsfxPluginInstance::collectMidiOutput(remidy::AudioProcessContext& process) {
        auto* fx = impl_->fx.get();
        auto& eventOut = process.eventOut();
        auto* buffer = static_cast<uint32_t*>(eventOut.getMessages());
        if (!buffer)
            return;
        size_t position = eventOut.position() / sizeof(uint32_t);
        const size_t capacity = eventOut.maxMessagesInBytes() / sizeof(uint32_t);

        ysfx_midi_event_t event{};
        while (position < capacity && ysfx_receive_midi(fx, &event)) {
            if (!event.data || event.size < 1)
                continue;
            const uint8_t status = event.data[0];
            // Only channel voice messages have a single-word UMP form. JSFX can emit
            // system and sysex messages too; those need a different message type and are
            // left for when something in the host wants them.
            if (status < 0x80 || status >= 0xF0)
                continue;
            const uint8_t data1 = event.size > 1 ? (event.data[1] & 0x7F) : 0;
            const uint8_t data2 = event.size > 2 ? (event.data[2] & 0x7F) : 0;
            // MIDI 1.0 channel voice UMP: message type 2, group 0.
            buffer[position++] =
                    (static_cast<uint32_t>(0x2) << 28) |
                    (static_cast<uint32_t>(status & 0xF0) << 16) |
                    (static_cast<uint32_t>(status & 0x0F) << 16) |
                    (static_cast<uint32_t>(data1) << 8) |
                    static_cast<uint32_t>(data2);
        }
        eventOut.position(position * sizeof(uint32_t));
    }

    std::vector<uapmd_plugin_hosting::ParameterMetadata> JsfxPluginInstance::parameterMetadataList() {
        std::vector<uapmd_plugin_hosting::ParameterMetadata> list{};
        for (auto* parameter : impl_->parameters->parameters()) {
            uapmd_plugin_hosting::ParameterMetadata metadata{};
            metadata.index = parameter->index();
            metadata.stableId = parameter->stableId();
            metadata.name = parameter->name();
            metadata.path = parameter->path();
            metadata.defaultPlainValue = parameter->defaultPlainValue();
            metadata.minPlainValue = parameter->minPlainValue();
            metadata.maxPlainValue = parameter->maxPlainValue();
            metadata.automatable = parameter->automatable();
            metadata.hidden = parameter->hidden();
            metadata.discrete = parameter->discrete();
            for (auto& entry : parameter->enums())
                metadata.namedValues.emplace_back(
                        uapmd_plugin_hosting::ParameterNamedValue{entry.value, entry.label});
            list.emplace_back(std::move(metadata));
        }
        return list;
    }

    std::vector<uint8_t> JsfxPluginInstance::saveStateSync() {
        if (!impl_->fx)
            return {};
        ysfx_state_u state{ysfx_save_state(impl_->fx.get())};
        if (!state)
            return {};
        return encodeState(*state);
    }

    void JsfxPluginInstance::loadStateSync(std::vector<uint8_t>& bytes) {
        if (!impl_->fx)
            return;
        DecodedState decoded{};
        if (!decodeState(bytes, decoded))
            return;

        ysfx_state_t state{};
        state.sliders = decoded.sliders.empty() ? nullptr : decoded.sliders.data();
        state.slider_count = static_cast<uint32_t>(decoded.sliders.size());
        state.data = decoded.data.empty() ? nullptr : decoded.data.data();
        state.data_size = decoded.data.size();
        ysfx_load_state(impl_->fx.get(), &state);

        // A script may declare different sliders than it did when the state was written,
        // so the parameter list is rebuilt rather than assumed to still match.
        impl_->parameters->rebuild();
    }

    double JsfxPluginInstance::getParameterValue(int32_t index) {
        double value{0};
        if (index < 0)
            return 0;
        impl_->parameters->getParameter(static_cast<uint32_t>(index), &value);
        return value;
    }

    void JsfxPluginInstance::setParameterValue(int32_t index, double value) {
        if (index >= 0)
            impl_->parameters->setParameter(static_cast<uint32_t>(index), value);
    }

    void JsfxPluginInstance::enqueueParameterValueRT(int32_t index, double value, uapmd_timestamp_t timestamp) {
        if (index >= 0)
            impl_->parameters->enqueueParameterRT(static_cast<uint32_t>(index), value,
                                                  static_cast<uint64_t>(timestamp));
    }

    std::string JsfxPluginInstance::getParameterValueString(int32_t index, double value) {
        if (index < 0)
            return {};
        return impl_->parameters->valueToString(static_cast<uint32_t>(index), value);
    }

    bool JsfxPluginInstance::hasUISupport() {
        return impl_->ui != nullptr;
    }

    bool JsfxPluginInstance::createUI(bool isFloating, void* parentHandle,
                                      std::function<bool(uint32_t, uint32_t)> resizeHandler) {
        // Nothing is created: the editor is a pixel buffer the host draws itself, reached
        // through PluginFramebufferUIExtension. Succeeding here rather than failing keeps
        // the usual show-the-editor flow working for a host that drives both kinds.
        (void) isFloating; (void) parentHandle; (void) resizeHandler;
        return impl_->ui != nullptr;
    }

    void JsfxPluginInstance::destroyUI() {
        if (!impl_->ui)
            return;
        impl_->ui_visible = false;
        impl_->ui->displayed(false);
        impl_->ui->stop();
    }

    bool JsfxPluginInstance::showUI() {
        if (!impl_->ui)
            return false;
        impl_->ui_visible = true;
        impl_->ui->displayed(true);
        // Drawing only happens while something is looking at it.
        impl_->ui->start();
        return true;
    }

    void JsfxPluginInstance::hideUI() {
        if (!impl_->ui)
            return;
        impl_->ui_visible = false;
        impl_->ui->displayed(false);
    }

    bool JsfxPluginInstance::isUIVisible() const {
        return impl_->ui_visible;
    }

    bool JsfxPluginInstance::canUIResize() {
        // The size the script asks for is a first-open hint; after that the host's size
        // decides and the script lays out against whatever gfx_w and gfx_h it is given.
        return impl_->ui != nullptr;
    }

    bool JsfxPluginInstance::getUISize(uint32_t& width, uint32_t& height) {
        return impl_->ui && impl_->ui->preferredSize(width, height);
    }

    bool JsfxPluginInstance::setUISize(uint32_t width, uint32_t height) {
        if (!impl_->ui)
            return false;
        impl_->ui->surfaceSize(width, height, 1.0);
        return true;
    }

    uapmd_plugin_hosting::AudioPluginInstanceExtension* JsfxPluginInstance::extension(
            std::string_view extensionId) {
        if (impl_->ui && extensionId == uapmd_plugin_hosting::kPluginFramebufferUIExtensionId)
            return impl_->ui.get();
        return nullptr;
    }

    remidy::PluginParameterSupport* JsfxPluginInstance::parameterSupport() {
        return impl_->parameters.get();
    }

    remidy::PluginAudioBuses* JsfxPluginInstance::audioBuses() {
        return impl_->buses.get();
    }

}
