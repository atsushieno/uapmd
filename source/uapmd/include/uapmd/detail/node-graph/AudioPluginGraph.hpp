#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <vector>

#include "uapmd/uapmd.hpp"
#include "AudioGraphExtension.hpp"
#include "AudioPluginNode.hpp"

namespace uapmd {
    // AudioPluginGraph is supposed to abstract away how it internally routes audio plugin instances.
    // It is supposed to `process()` audio and UMP inputs. In that sense, it is a track on a DAW alike.
    //
    // It can process nodes in a simple linear way. As a minimum requirement, it must support
    // a linear chain of nodes, by "add" and "remove" functions.
    // Advanced DAG topology editing lives in AudioPluginFullDAGraph.
    //
    // Note that it is NOT to represent an entire audio graph that is supposed to involve audio device nodes.
    // That should be handled by a sequencer, which is out of the scope of this class (not even of this library).
    //
    // Usually an audio plugin instance shares its lifetime with `AudioPluginNode` when it is being added to the graph,
    // but the instance is "owned" by the `AudioPluginHostingAPI` implementation.
    // `appendNodeSimple()` takes a delegate function called when the instance is being removed by `removeNodeSimple()`
    // or whatever that removes `AudioPluginNode`, to help this mechanism.
    class AudioPluginGraph {
    protected:
        explicit AudioPluginGraph(std::string providerId = {})
            : provider_id_(std::move(providerId)) {}
        virtual AudioGraphExtension* getExtension(const std::type_info& type) = 0;
        virtual const AudioGraphExtension* getExtension(const std::type_info& type) const = 0;

    public:
        virtual ~AudioPluginGraph() = default;

        const std::string& providerId() const {
            return provider_id_;
        }
        virtual uapmd_status_t appendNodeSimple(int32_t instanceId, AudioPluginInstanceAPI* instance, std::function<void()>&& onDelete) = 0;
        virtual bool removeNodeSimple(int32_t instanceId) = 0;

        virtual std::map<int32_t, AudioPluginNode*> plugins() = 0;
        virtual AudioPluginNode* getPluginNode(int32_t instanceId) = 0;

        // FIXME: in the end, we should move it to "sequencer" which has to dispatch the combined UMP inputs
        // to the "track" to each "mapped" Function Block by group ID (which identifies FB).
        // Each AudioPluginNode can be enqueued UMP input directly, and we already do so for direct inputs
        // on uapmd-app.
        virtual void setGroupResolver(std::function<uint8_t(int32_t)> resolver) = 0;

        //
        virtual void setEventOutputCallback(std::function<void(int32_t instanceId, const uapmd_ump_t* data, size_t dataSizeInBytes)> callback) = 0;

        virtual int32_t processAudio(AudioProcessContext& process) = 0;
        // Number of currently addressable graph output buses for latency/tail queries.
        // Future graph implementations may expose different values per output bus.
        virtual uint32_t outputBusCount() = 0;
        virtual uint32_t outputLatencyInSamples(uint32_t outputBusIndex) = 0;
        virtual double outputTailLengthInSeconds(uint32_t outputBusIndex) = 0;
        // Scheduling-oriented render lead for the graph. Current linear graphs use the
        // maximum reported output latency, while future DAG graphs may compute a richer
        // topology-aware lead without changing sequencer code.
        virtual uint32_t renderLeadInSamples() = 0;
        virtual uint32_t mainOutputLatencyInSamples() = 0;
        virtual double mainOutputTailLengthInSeconds() = 0;

        template <typename T>
        T* getExtension() {
            static_assert(std::is_base_of_v<AudioGraphExtension, T>);
            return dynamic_cast<T*>(getExtension(typeid(T)));
        }

        template <typename T>
        const T* getExtension() const {
            static_assert(std::is_base_of_v<AudioGraphExtension, T>);
            return dynamic_cast<const T*>(getExtension(typeid(T)));
        }

        static std::unique_ptr<AudioPluginGraph> create(size_t eventBufferSizeInBytes);
        static bool migrate(AudioPluginGraph& to, AudioPluginGraph& from);

    private:
        virtual std::vector<std::shared_ptr<AudioPluginNode>> releaseNodesForMigration() = 0;
        virtual bool adoptNodesFromMigration(std::vector<std::shared_ptr<AudioPluginNode>>&& nodes) = 0;
        std::string provider_id_{};
    };

}
