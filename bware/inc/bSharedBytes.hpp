#ifndef NFSHP2_BSHAREDBYTES_HPP
#define NFSHP2_BSHAREDBYTES_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace hp2 {
// One decoded resource file, shared read-only by every owner that retains it
// (the loader, track scenario, lights, models, markers, animations). Owners
// used to keep private copies of the same bytes; constructing from a vector
// still copies, so only callers that pass a SharedBytes share the buffer.
class SharedBytes {
public:
    using Bytes = std::vector<std::uint8_t>;
    SharedBytes() = default;
    SharedBytes(const Bytes& bytes) : bytes_(std::make_shared<const Bytes>(bytes)) {}
    SharedBytes(Bytes&& bytes) : bytes_(std::make_shared<const Bytes>(std::move(bytes))) {}
    const Bytes& get() const noexcept { return bytes_ ? *bytes_ : Empty(); }
    operator const Bytes&() const noexcept { return get(); }
    const std::uint8_t* data() const noexcept { return get().data(); }
    std::size_t size() const noexcept { return get().size(); }
    bool empty() const noexcept { return get().empty(); }
    Bytes::const_iterator begin() const noexcept { return get().begin(); }
    Bytes::const_iterator end() const noexcept { return get().end(); }
    const std::uint8_t& operator[](std::size_t index) const noexcept { return get()[index]; }
private:
    static const Bytes& Empty() noexcept { static const Bytes empty; return empty; }
    std::shared_ptr<const Bytes> bytes_;
};
} // namespace hp2
using hp2::SharedBytes;

#endif
