#include "pipeline_cache.hpp"
namespace hp2::nx {
#ifndef __SWITCH2__
#include "pipeline_cache_seed.inc"
#endif
PipelineCacheSeed BundledPipelineCache() noexcept
{
#ifdef __SWITCH2__
    // The bundled Maxwell cache belongs to Switch 1, not Ampere/VI2.
    return {};
#else
    return {embeddedPipelineCache,sizeof(embeddedPipelineCache)};
#endif
}
}
