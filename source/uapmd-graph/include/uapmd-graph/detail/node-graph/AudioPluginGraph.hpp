#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <vector>

#include "AudioGraph.hpp"
#include "AudioGraphDescriptor.hpp"
#include "AudioGraphRegistry.hpp"
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
    class AudioPluginGraph : public AudioGraph {
    protected:
        explicit AudioPluginGraph(std::string providerId = {})
            : AudioGraph(std::move(providerId)) {}

    public:
        virtual ~AudioPluginGraph() = default;

        virtual uapmd_status_t appendNodeSimple(int32_t instanceId, AudioPluginInstanceAPI* instance, std::function<void()>&& onDelete) = 0;
        virtual uapmd_status_t appendBuiltInNodeSimple(const AudioGraphNodeDescriptor& descriptor) = 0;
        virtual bool removeNodeSimple(int32_t instanceId) = 0;

        virtual std::map<int32_t, AudioPluginNode*> plugins() = 0;
        virtual AudioPluginNode* getPluginNode(int32_t instanceId) = 0;

        static std::unique_ptr<AudioPluginGraph> create(size_t eventBufferSizeInBytes);
        static bool migrate(AudioPluginGraph& to, AudioPluginGraph& from);

    private:
        virtual std::vector<std::shared_ptr<AudioGraphNode>> releaseNodesForMigration() = 0;
        virtual bool adoptNodesFromMigration(std::vector<std::shared_ptr<AudioGraphNode>>&& nodes) = 0;
    };

}
