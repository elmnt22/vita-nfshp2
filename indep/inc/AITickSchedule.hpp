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
// The slots above advance once per retail world loop, which ran at the
// game's 30 frames a second. The native world steps at 60 Hz, so counting
// its loops thinks every car twice as often. Each think re-seeds a far
// racer's BeltMover from its current speed (and the belt gains on it), so
// doubled thinks made rivals run away to several hundred km/h (GitHub
// issue 1). Turns therefore follow world time in thirtieths of a second:
// a step covering sixtieths [firstFrame, firstFrame+frames) owns the turns
// t with 2t in that span (one per 1/30 s step, every other 1/60 s step).
constexpr bool GroupCarDueInSpan(std::int32_t firstFrame,std::int32_t frames,std::int32_t groupIndex,
    std::int32_t groupCount,std::int32_t retailWindow) noexcept
{
    if(frames<=0 || firstFrame<0)return false;
    const std::int32_t firstTurn=(firstFrame+1)/2,lastTurn=(firstFrame+frames-1)/2;
    for(std::int32_t turn=firstTurn,seen=0;turn<=lastTurn && seen<retailWindow;++turn,++seen)
        if(GroupCarDue(turn,groupIndex,groupCount,retailWindow))return true;
    return false;
}
// 1 (default): retail-rate turns by world time; 0: one turn per world loop.
extern int retailThinkTurns;
} // namespace hp2::ai
