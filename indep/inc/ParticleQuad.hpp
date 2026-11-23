#pragma once
#include <array>
#include <cstdint>
namespace hp2 {
// Native projected particle quad (see Ecstasy.hpp's particle transport).
struct EcstasyParticleQuad {
    std::array<std::array<float,2>,4> clipXY{};
    std::array<std::array<float,2>,4> uv{};
    float depth{};
    std::uint32_t color{};
};
}
