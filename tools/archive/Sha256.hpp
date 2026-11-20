#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace hp2archive {

// SHA-256 is used only by the offline archive utility. The game has no dependency
// on this file. State and message arithmetic are the unsigned operations in FIPS 180-4.
class Sha256 final {
public:
    void Update(const void* data, std::size_t size) noexcept
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        total_ += static_cast<std::uint64_t>(size);
        if (buffered_ != 0) {
            const std::size_t take = size < block_.size() - buffered_ ? size : block_.size() - buffered_;
            if (take != 0) std::memcpy(block_.data() + buffered_, bytes, take);
            buffered_ += take;
            bytes += take;
            size -= take;
            if (buffered_ == block_.size()) { Transform(block_.data()); buffered_ = 0; }
            else return;
        }
        while (size >= block_.size()) {
            Transform(bytes);
            bytes += block_.size();
            size -= block_.size();
        }
        if (size != 0) std::memcpy(block_.data(), bytes, size);
        buffered_ = size;
    }

    std::string Hex() const
    {
        Sha256 completed = *this;
        const std::uint64_t bits = total_ * 8;
        const std::uint8_t marker = 0x80;
        completed.Update(&marker, 1);
        const std::uint8_t zero = 0;
        while (completed.buffered_ != 56) completed.Update(&zero, 1);
        std::array<std::uint8_t, 8> length{};
        for (std::size_t i = 0; i != length.size(); ++i)
            length[7 - i] = static_cast<std::uint8_t>(bits >> (i * 8));
        completed.Update(length.data(), length.size());
        static constexpr char digits[] = "0123456789abcdef";
        std::string output(64, '0');
        for (std::size_t i = 0; i != completed.state_.size(); ++i) {
            for (std::size_t j = 0; j != 4; ++j) {
                const auto value = static_cast<std::uint8_t>(completed.state_[i] >> ((3 - j) * 8));
                output[i * 8 + j * 2] = digits[value >> 4];
                output[i * 8 + j * 2 + 1] = digits[value & 15];
            }
        }
        return output;
    }

private:
    static std::uint32_t Rotate(std::uint32_t word, unsigned shift) noexcept
    {
        return (word >> shift) | (word << (32 - shift));
    }

    void Transform(const std::uint8_t* bytes) noexcept
    {
        static constexpr std::array<std::uint32_t, 64> constants{{
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
        }};
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i != 16; ++i) {
            words[i] = (std::uint32_t{bytes[i * 4]} << 24) | (std::uint32_t{bytes[i * 4 + 1]} << 16)
                     | (std::uint32_t{bytes[i * 4 + 2]} << 8) | std::uint32_t{bytes[i * 4 + 3]};
        }
        for (std::size_t i = 16; i != words.size(); ++i) {
            const auto a = words[i - 15];
            const auto b = words[i - 2];
            words[i] = words[i - 16] + (Rotate(a, 7) ^ Rotate(a, 18) ^ (a >> 3))
                     + words[i - 7] + (Rotate(b, 17) ^ Rotate(b, 19) ^ (b >> 10));
        }
        auto a=state_[0],b=state_[1],c=state_[2],d=state_[3];
        auto e=state_[4],f=state_[5],g=state_[6],h=state_[7];
        for (std::size_t i = 0; i != words.size(); ++i) {
            const auto first = h + (Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25))
                             + ((e & f) ^ (~e & g)) + constants[i] + words[i];
            const auto second = (Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22))
                              + ((a & b) ^ (a & c) ^ (b & c));
            h=g; g=f; f=e; e=d+first; d=c; c=b; b=a; a=first+second;
        }
        state_[0]+=a;state_[1]+=b;state_[2]+=c;state_[3]+=d;
        state_[4]+=e;state_[5]+=f;state_[6]+=g;state_[7]+=h;
    }

    std::array<std::uint32_t, 8> state_{{
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    }};
    std::array<std::uint8_t, 64> block_{};
    std::size_t buffered_{};
    std::uint64_t total_{};
};

} // namespace hp2archive
