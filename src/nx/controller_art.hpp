#pragma once

#include <cstdint>
#include <vector>

namespace hp2::nx {
void OverrideControllerTexture(std::uint32_t key,std::uint32_t width,std::uint32_t height,
    std::vector<std::uint8_t>& rgba);
}
