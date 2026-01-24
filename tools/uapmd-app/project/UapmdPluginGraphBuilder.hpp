#include <uapmd/uapmd.hpp>
#include "UapmdSequence.hpp"

namespace uapmd {
    class UapmdPluginGraphBuilder {
    public:
        static std::unique_ptr<AudioPluginGraph> build(UapmdProjectPluginGraphData* data);
    };

    class UapmdSequenceBuilder {
    public:
        static std::unique_ptr<UapmdSequence> build(UapmdProjectData* data);
    };
}
