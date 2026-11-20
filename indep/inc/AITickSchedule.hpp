#pragma once
#include <cstdint>

namespace hp2::ai {
// PAL schedules one of at most eight opponent or six traffic slots in each
// fixed-width simulation cycle. The native PC population can exceed the
// physical PS2 window. Assign surplus cars to an existing bucket instead
// of stretching the update period or permanently starving those cars.
// Every call is driven by worldLoopCounter, never by display FPS.
constexpr bool GroupCarDue(std::int32_t worldLoop,std::int32_t groupIndex,
    std::int32_t groupCount,std::int32_t retailWindow) noexcept
{
    return worldLoop>=0 && groupCount>0 && retailWindow>0 &&
        groupIndex>=0 && groupIndex<groupCount &&
        groupIndex%retailWindow==worldLoop%retailWindow;
}
} // namespace hp2::ai
