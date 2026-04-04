#include <uapmd/uapmd.hpp>
#include "UapmdProjectFile.hpp"
#include "UapmdSequence.hpp"

namespace uapmd {
    class UapmdPluginGraphBuilder {
    public:
        static bool build(
            UapmdProjectPluginGraphData* data,
            AudioPluginGraph& graph,
            const std::vector<int32_t>& orderedInstanceIds);
    };

    class UapmdAudioPluginFullDAGraphBuilder {
    public:
        static bool build(
            UapmdProjectPluginGraphData* data,
            AudioPluginGraph& graph,
            const std::vector<int32_t>& orderedInstanceIds);
    };

    class UapmdSequenceBuilder {
    public:
        static std::unique_ptr<UapmdSequence> build(UapmdProjectData* data);
    };
}
