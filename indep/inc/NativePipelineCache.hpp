#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hp2 {
struct NativePipelineCacheIdentity {
    std::uint32_t vendor{},device{};
    std::array<std::uint8_t,16> uuid{};
};
inline bool NativePipelineCacheMatches(const void* bytes,std::size_t size,
    const NativePipelineCacheIdentity& identity) noexcept
{
    if(!bytes || size<32 || size>16*1024*1024)return false;
    const auto* data=static_cast<const std::uint8_t*>(bytes);
    const auto word=[&](std::size_t at) {
        return std::uint32_t(data[at]) | (std::uint32_t(data[at+1])<<8) |
            (std::uint32_t(data[at+2])<<16) | (std::uint32_t(data[at+3])<<24);
    };
    // VkPipelineCacheHeaderVersionOne uses little-endian integers even when
    // the host is not little-endian. Opaque payload validation stays with Vulkan.
    return word(0)==32 && word(4)==1 && word(8)==identity.vendor && word(12)==identity.device &&
        std::memcmp(data+16,identity.uuid.data(),identity.uuid.size())==0;
}

template<class Handle,class Create,class Merge,class Destroy>
Handle CreateNativePipelineCache(const void* saved,std::size_t savedSize,
    const void* bundled,std::size_t bundledSize,const NativePipelineCacheIdentity& identity,
    Create&& create,Merge&& merge,Destroy&& destroy)
{
    Handle cache{};
    const bool savedValid=NativePipelineCacheMatches(saved,savedSize,identity);
    const bool bundledValid=NativePipelineCacheMatches(bundled,bundledSize,identity);
    if(savedValid)cache=create(saved,savedSize);
    if(bundledValid) {
        if(!cache)cache=create(bundled,bundledSize);
        else if(!savedValid || savedSize!=bundledSize || std::memcmp(saved,bundled,savedSize)!=0) {
            // Merge only during device startup, before any pipeline worker uses
            // the cache. Retain the saved cache if an optional seed is rejected.
            if(Handle seed=create(bundled,bundledSize)) {merge(cache,seed);destroy(seed);}
        }
    }
    if(!cache)cache=create(nullptr,0);
    return cache;
}
}
