#pragma once

#include "indep/inc/CarDriver.hpp"

namespace hp2::nx {
inline bool drivingControls=false;

constexpr platform::Ps2Button ContextButton(platform::Ps2Button button,bool driving) noexcept
{
    using platform::Ps2Button;
    const auto alternate=driving?Ps2Button::Square:Ps2Button::Triangle;
    if(button==Ps2Button::Circle)return alternate;
    if(button==alternate)return Ps2Button::Circle;
    return button;
}

constexpr unsigned char ContextGlyph(unsigned char glyph,bool driving) noexcept
{
    // BUTTONSOUT cells: H = Square, J = Triangle, K = Circle.
    const auto alternate=driving?'h':'j';
    if(glyph=='k')return static_cast<unsigned char>(alternate);
    if(glyph==alternate)return 'k';
    return glyph;
}
}
