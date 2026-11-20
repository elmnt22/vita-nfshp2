#pragma once

#include <chrono>
#include <filesystem>
#include <iosfwd>
#include <string>

namespace hp2::nx {

class PgoSession {
public:
    PgoSession(const std::filesystem::path& saves,std::ostream& log) noexcept
        :saves_(saves),log_(log) {}
    static const char* BuildMode() noexcept;
    void Checkpoint(bool force=false) noexcept;

private:
    const std::filesystem::path& saves_;
    std::ostream& log_;
    std::chrono::steady_clock::time_point nextCheckpoint_{};
    std::string session_;
    unsigned checkpoint_{};
};

}
