#pragma once
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "uapmd/uapmd.hpp"
#include "AudioPluginNode.hpp"

namespace uapmd {

    // AudioPluginGraph is supposed to abstract away how it internally routes audio plugin instances.
    // It is supposed to `process()` audio and UMP inputs. In that sense, it is a track on a DAW alike.
    //
    // It can process nodes in a simple linear way, or in a perfect DAG that handles complex routing.
    // As a minimum requirement, it must support a linear chain of nodes, by "add" and "remove" functions.
    //
    // Usually an audio plugin instance shares its lifetime with `AudioPluginNode` when it is being added to the graph,
    // but the instance is "owned" by the `AudioPluginHostingAPI` implementation.
    // `appendNodeSimple()` takes a delegate function called when the instance is being removed by `removeNodeSimple()`
    // or whatever that removes `AudioPluginNode`, to help this mechanism.
    class AudioPluginGraph {
    protected:
        AudioPluginGraph() = default;

    public:
        virtual ~AudioPluginGraph() = default;

        virtual uapmd_status_t appendNodeSimple(int32_t instanceId, AudioPluginInstanceAPI* instance, std::function<void()>&& onDelete) = 0;
        virtual bool removeNodeSimple(int32_t instanceId) = 0;

        // FIXME: we are examining the need for this function. If possible, remove it.
        virtual std::map<int32_t, AudioPluginNode*> plugins() = 0;

        // FIXME: in the end, we should move it to "sequencer" which has to dispatch the combined UMP inputs
        // to the "track" to each "mapped" Function Block by group ID (which identifies FB).
        // Each AudioPluginNode can be enqueued UMP input directly, and we already do so for direct inputs
        // on uapmd-app.
        virtual void setGroupResolver(std::function<uint8_t(int32_t)> resolver) = 0;
        virtual void setEventOutputCallback(std::function<void(int32_t, const uapmd_ump_t*, size_t)> callback) = 0;

        virtual int32_t processAudio(AudioProcessContext& process) = 0;

        // Creates a minimum linear implementation of this interface.
        static std::unique_ptr<AudioPluginGraph> create(size_t eventBufferSizeInBytes);
    };

}
