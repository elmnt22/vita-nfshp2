#pragma once
#include <cstddef>
#include <cstdint>
namespace hp2::nx {
struct PipelineCacheSeed {const std::uint8_t* data;std::size_t size;};
PipelineCacheSeed BundledPipelineCache() noexcept;
}
