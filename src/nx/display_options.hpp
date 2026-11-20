#pragma once

#include <cstdint>
namespace hp2::frontend {struct FlailerUiState;class FlailerResources;}
namespace hp2::nx {
inline constexpr std::uint32_t frontendDisplay=0x8bc68340u,pauseDisplay=0xcc36fab2u;
inline constexpr std::uint32_t frameLimitValue=0xf1010010u,showFpsValue=0xf1010011u;
std::uint32_t FrameLimitItem(std::uint32_t screen) noexcept;
std::uint32_t ShowFpsItem(std::uint32_t screen) noexcept;
std::uint32_t DisplayMenuItem(std::uint32_t screen,std::uint32_t ordinal,std::uint32_t fallback) noexcept;
int DisplayMenuPosition(std::uint32_t screen,std::uint32_t key) noexcept;
void PrepareDisplayMenu(frontend::FlailerUiState& ui,std::uint32_t screen);
void RefreshDisplayMenu(frontend::FlailerResources& resources,std::uint32_t screen);
bool HandleDisplaySetting(frontend::FlailerResources& resources,std::uint32_t screen,std::uint32_t key,int direction);
}
