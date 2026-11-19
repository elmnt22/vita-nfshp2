// Plain-data stand-ins for the few Vulkan names used by the API-neutral
// renderer helpers (vertex/batch preparation, texture keys, geometry caches).
// Used by the non-Vulkan (vitaGL) backend: handles are opaque tokens that the
// GL Storage fills with its own object ids; nothing here calls a GPU API.
#ifndef NFSHP2_SRC_ECSTASY_VK_POD_HPP
#define NFSHP2_SRC_ECSTASY_VK_POD_HPP

#include <cstdint>

#define HP2_VK_POD_HANDLE(name) typedef struct name##_T* name;
HP2_VK_POD_HANDLE(VkBuffer)
HP2_VK_POD_HANDLE(VkDeviceMemory)
HP2_VK_POD_HANDLE(VkImage)
HP2_VK_POD_HANDLE(VkImageView)
HP2_VK_POD_HANDLE(VkSampler)
HP2_VK_POD_HANDLE(VkDescriptorSet)
HP2_VK_POD_HANDLE(VkDescriptorPool)
HP2_VK_POD_HANDLE(VkFramebuffer)
HP2_VK_POD_HANDLE(VkPipeline)
#undef HP2_VK_POD_HANDLE

#define VK_NULL_HANDLE nullptr

typedef std::uint64_t VkDeviceSize;
typedef std::int32_t VkResult;
constexpr VkResult VK_SUCCESS = 0;

struct VkOffset2D { std::int32_t x, y; };
struct VkExtent2D { std::uint32_t width, height; };
struct VkRect2D { VkOffset2D offset; VkExtent2D extent; };
struct VkViewport { float x, y, width, height, minDepth, maxDepth; };
struct VkBufferCopy { VkDeviceSize srcOffset, dstOffset, size; };
struct VkDrawIndexedIndirectCommand {
    std::uint32_t indexCount, instanceCount, firstIndex;
    std::int32_t vertexOffset;
    std::uint32_t firstInstance;
};

#endif
